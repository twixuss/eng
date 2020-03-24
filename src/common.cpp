#include "common.h"

#pragma warning(push, 0)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <strsafe.h>
#pragma warning(pop)

#if IS_WINDOWS
i64 const PerfTimer::frequency = []() {
	LARGE_INTEGER freq;
	QueryPerformanceFrequency(&freq);
	return freq.QuadPart;
}();
i64 PerfTimer::getCounter() {
	LARGE_INTEGER counter;
	QueryPerformanceCounter(&counter);
	return counter.QuadPart;
}
#else
#error no timer
#endif

i64 setFilePointer(void* file, i64 distance, FilePoint startPoint) {
	LARGE_INTEGER result, dist;
	dist.QuadPart = distance;
	if (!SetFilePointerEx(file, dist, &result, (DWORD)startPoint)) {
		return -1;
	}
	return result.QuadPart;
}

i64 getFilePointer(void* file) {
	LARGE_INTEGER result;
	if (!SetFilePointerEx(file, {}, &result, SEEK_CUR)) {
		return -1;
	}
	return result.QuadPart;
}

bool readFile(void* file, void* buffer, size_t size) {
	DWORD const maxSize = 0xFFFFFFFF;
	for (;;) {
		DWORD bytesRead = 0;
		if (size > maxSize) {
			if (!ReadFile(file, buffer, maxSize, &bytesRead, 0)) {
				return false;
			}
			buffer = (u8*)buffer + maxSize;
			size -= maxSize;
		} else {
			if (!ReadFile(file, buffer, (DWORD)size, &bytesRead, 0)) {
				return false;
			}
			break;
		}
	}
	return true;
}

ReadFileResult readFile(HANDLE handle) {
	LARGE_INTEGER endPtr{};
	if (!SetFilePointerEx(handle, {}, &endPtr, FILE_END)) {
		return {};
	}
	if (!SetFilePointerEx(handle, {}, 0, FILE_BEGIN)) {
		return {};
	}
	size_t size = (size_t)endPtr.QuadPart;
	auto data = malloc(size);
	if (!readFile(handle, data, size)) {
		free(data);
		return {};
	}
	return {{(char*)data, size}};
}
ReadFileResult readFile(wchar const* path) {
	HANDLE handle = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
	if (handle == INVALID_HANDLE_VALUE) {
		return {};
	}
	DEFER { CloseHandle(handle); };
	return readFile(handle);
}
ReadFileResult readFile(char const* path) {
	HANDLE handle = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
	if (handle == INVALID_HANDLE_VALUE) {
		return {};
	}
	DEFER { CloseHandle(handle); };
	return readFile(handle);
}
bool writeFile(void* file, void const* buffer, size_t size) {
	auto handle = (HANDLE)file;
	DWORD const maxSize = 0xFFFFFFFF;
	for (;;) {
		DWORD bytesWritten = 0;
		if (size > maxSize) {
			if (!WriteFile(handle, buffer, maxSize, &bytesWritten, 0)) {
				return false;
			}
			buffer = (u8*)buffer + maxSize;
			size -= maxSize;
		} else {
			if (!WriteFile(handle, buffer, (DWORD)size, &bytesWritten, 0)) {
				return false;
			}
			break;
		}
	}
	return true;
}

void* copyBuffer(void const* data, size_t size) {
	auto result = malloc(size);
	memcpy(result, data, size);
	return result;
}
void freeCopiedBuffer(void* data) { free(data); }
namespace Profiler {
HANDLE thread;
SPSC::CircularQueue<Entry, 256> entries;
bool volatile stopped;
void push(Entry e) {
	e.end = PerfTimer::getCounter();
	entries.push(e);
}
void init() {
	thread = CreateThread(
		0, 0,
		[](void*) -> DWORD {
			HANDLE file = CreateFileA("profile.json", GENERIC_WRITE, 0, 0, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
			ASSERT(file != INVALID_HANDLE_VALUE);
			char const* header = R"({"otherData":{},"traceEvents":[)";
			writeFile(file, header, strlen(header));

			for (;;) {
				if (auto e = entries.pop(); e.has_value()) {
					i64 end = e->end;
					i64 begin = e->begin;
					i64 const usInSecond = 1000000;
					begin = begin * usInSecond / PerfTimer::frequency;
					end = end * usInSecond / PerfTimer::frequency;
					char buffer[256];
					ASSERT(StringCbPrintfA(
							   buffer, _countof(buffer),
							   R"({"cat":"function","dur":%lli,"name":"%s","ph":"X","pid":0,"tid":%u,"ts":%lli},)",
							   end - begin, e->name, GetCurrentThreadId(), begin) == S_OK);
					writeFile(file, buffer, strlen(buffer));
					free(e->name);
				} else if (stopped) {
					break;
				} else {
					Sleep(1);
				}
			}
			char const* footer = "]}";
			setFilePointer(file, -1, FilePoint::cursor);
			writeFile(file, footer, strlen(footer));
			CloseHandle(file);
			return 0;
		},
		0, 0, 0);
	ASSERT(thread);
}
void shutdown() {
	stopped = true;
	WaitForSingleObjectEx(thread, INFINITE, false);
	CloseHandle(thread);
}
} // namespace Profiler