#pragma once
#include <cstdint>
#include <cstdio>
#include <cstdarg>
#include <cstdlib>
inline unsigned long millis(){return 10000;}
struct FakeSerial {
 template<typename... Ts> void printf(const char* fmt, Ts... args){std::printf(fmt,args...);}
 void println(const char* s){std::puts(s);}
};
inline FakeSerial Serial;
