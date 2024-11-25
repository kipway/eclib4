/*
\file ec_new.h
\author jiangyong
\email  kipway@outlook.com

eclib 4.0 Copyright (c) 2017-2024, kipway
source repository : https://github.com/kipway

Licensed under the Apache License, Version 2.0 (the "License");
You may obtain a copy of the License at http://www.apache.org/licenses/LICENSE-2.0
*/

#pragma once
#ifdef _WIN32 // windows版支持C++14
#define DECLARE_EC_GLOABL_NEW_DEL void* operator new (std::size_t size){return get_ec_allocator()->malloc_(size);}\
void* operator new (std::size_t size, const std::nothrow_t& nothrow_value) noexcept{return get_ec_allocator()->malloc_(size);}\
void operator delete (void* ptr) noexcept{ get_ec_allocator()->free_(ptr);}\
void operator delete (void* ptr, const std::nothrow_t& nothrow_constant) noexcept{ get_ec_allocator()->free_(ptr);}\
void operator delete (void* ptr, std::size_t size) noexcept { get_ec_allocator()->free_(ptr); }\
void operator delete (void* ptr, std::size_t size, const std::nothrow_t& nothrow_constant) noexcept { get_ec_allocator()->free_(ptr);}

void* operator new (std::size_t size);
void* operator new (std::size_t size, const std::nothrow_t& nothrow_value) noexcept;
void operator delete (void* ptr) noexcept;
void operator delete (void* ptr, const std::nothrow_t& nothrow_constant) noexcept;
//{{ C++14
void operator delete (void* ptr, std::size_t size) noexcept;
void operator delete (void* ptr, std::size_t size, const std::nothrow_t& nothrow_constant) noexcept;
//}}

#else // linux 仅用C++11
#define DECLARE_EC_GLOABL_NEW_DEL void* operator new (std::size_t size){return get_ec_allocator()->malloc_(size);}\
void* operator new (std::size_t size, const std::nothrow_t& nothrow_value) noexcept{return get_ec_allocator()->malloc_(size);}\
void operator delete (void* ptr) noexcept{ get_ec_allocator()->free_(ptr);}\
void operator delete (void* ptr, const std::nothrow_t& nothrow_constant) noexcept{ get_ec_allocator()->free_(ptr);}

void* operator new (std::size_t size);
void* operator new (std::size_t size, const std::nothrow_t& nothrow_value) noexcept;
void operator delete (void* ptr) noexcept;
void operator delete (void* ptr, const std::nothrow_t& nothrow_constant) noexcept;
#endif