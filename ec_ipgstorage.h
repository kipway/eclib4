/**
\brief ec_ipgstorage.h
interface of storage

\author	jiangyong
\email  kipway@outlook.com
\update 
  2025.6.18  增加页面大小接口
  2022.10.18 初版

eclib 3.0 Copyright (c) 2017-2021, kipway
source repository : https://github.com/kipway

Licensed under the Apache License, Version 2.0 (the "License");
You may obtain a copy of the License at http://www.apache.org/licenses/LICENSE-2.0
*/
#pragma once
#include <stdint.h>

#define EC_PGF_ENDNO (-1)  //结束页面号

#define EC_PGF_MINPAGESIZE 128 //最小页面大小

#ifndef EC_PGF_MAXPAGESIZE
#define EC_PGF_MAXPAGESIZE (1024 * 32) //最大页面大小
#endif

#define EC_PGF_SUCCESS    0    //成功
#define EC_PGF_FAILED     (-1) //失败
#define EC_PGF_ERR_IO     (-2) //存储IO错误
#define EC_PGF_ERR_ALLOC  (-3) //分配页面失败
#define EC_PGF_ERR_READ   (-4) //读页面失败
#define EC_PGF_ERR_WRITE  (-5) //写页面失败
#define EC_PGF_ERR_PAGE   (-6) //页面错误(页面头或者数据解析)
#define EC_PGF_ERR_FREE   (-7) //释放页面错误
#define EC_PGF_ERR_MEMORY (-8) //内存错误
#define EC_PGF_ERR_PGFULL (-9) //分配页面失败,满

namespace ec {
	/**
	 * @brief 页面存储接口，只需实现4个接口，以便实现具体存储层的可替换应用。
	*/
	class ipage_storage //页面存储接口
	{
	public:
		virtual size_t pg_size() = 0; //返回页面大小
		virtual int64_t pg_alloc() = 0;// 分配一新的页面,返回页面号,-1表示失败
		virtual bool pg_free(int64_t pgno) = 0;// 删除页面
		virtual int  pg_read(int64_t pgno, size_t offset, void* pbuf, size_t bufsize) = 0;// 读页面，返回读取到的字节数，-1表示失败
		virtual int  pg_write(int64_t pgno, size_t offset, const void* pdata, size_t datasize) = 0;// 写页面, 返回写入字节数, -1表示失败
		virtual ~ipage_storage() {}
	};
}// namespace