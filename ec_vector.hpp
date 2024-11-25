/*!
\file ec_vector.h
\author	jiangyong
\email  kipway@outlook.com
\update
  2024.11.9 support none ec_alloctor

std::vector use ec::std_allocator

eclib 4.0 Copyright (c) 2017-2024, kipway
source repository : https://github.com/kipway

Licensed under the Apache License, Version 2.0 (the "License");
You may obtain a copy of the License at http://www.apache.org/licenses/LICENSE-2.0
*/
#pragma once
#include <vector>
namespace ec
{
#ifdef _HAS_EC_ALLOCTOR
	template<typename _Tp>
	struct vector : std::vector<_Tp, ec::std_allocator<_Tp>> {
		using std::vector<_Tp, ec::std_allocator<_Tp>>::vector;
	};
#else
	template<typename _Tp>
	struct vector : std::vector<_Tp> {
		using std::vector<_Tp>::vector;
	};
#endif
}
