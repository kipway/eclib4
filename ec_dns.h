/*!
\file ec_dns.h
  DNS相关的，用于实现DNS服务端, 遵循标准 RFC1034, RFC1035, RFC3596

\author jiangyong
\date
	2024-11-12 always use ec::string and ec::bytes

eclib 4.0 Copyright (c) 2017-2024, kipway
source repository : https://github.com/kipway

Licensed under the Apache License, Version 2.0 (the "License");
You may obtain a copy of the License at http://www.apache.org/licenses/LICENSE-2.0
*/

#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "ec_stream.h"
#include "ec_log.h"

using dnsString = ec::string;
using dnsBytes = ec::bytes;

#ifndef EC_DNS_TTL
#define EC_DNS_TTL 20
#endif

namespace ec
{
	/**
	 * @brief DNS报文头部
	 * https://datatracker.ietf.org/doc/html/rfc1035
	 */
	struct dnsPkgHead
	{
		uint16_t _id;//16bit  报文ID，用于请求应答，客户端生成并设置，服务端原样返回.

		uint16_t  _qr : 1; //1bit Query（0） Or response(1) 
		uint16_t  _optcode : 4;//4bit ,客户端设置，服务端原样返回。
		// 0	Query（最常用的查询）[RFC1035]
		// 1	IQuery(反向查询，现在已经不再使用了)[RFC3425]
		// 2	Status[RFC1035]
		// 3	未指定
		// 4	Notify[RFC1996]
		// 5	Update[RFC2136]
		// 6	DNS Stateful Operations(DSO)[RFC8490]
		uint16_t _aa : 1;//1bit, 权威应答标记，当响应报文由权威服务器发出时，该位置1，否则为0。
		uint16_t _tc : 1;//1bit, 当使用UDP传输时, 若响应数据超过DNS标准限制（超过512B），数据包便会发生截断，超出部分被丢弃，此时该flag位被置1。
		//当客户端发现TC位被置1的响应数据包时应该选择使用TCP重新发送查询。因为TCP DNS报文不受512字节限制。
		uint16_t _rd : 1;//1bit 客户端希望服务器对此次查询进行递归查询时将该位置1，否则置0。响应时RD位会复制到响应报文内。

		uint16_t _ra : 1;//1bit 服务器根据自己是否支持递归查询对该位进行设置。1为支持递归查询，0为不支持递归查询。
		uint16_t _res : 3;//3bit 保留0。
		uint16_t _rcode : 4;//4bit,
		// 0	没有错误。[RFC1035]
		// 1	Format error：格式错误，服务器不能理解请求的报文格式。[RFC1035]
		// 2	Server failure：服务器失败，因为服务器的原因导致没办法处理这个请求。[RFC1035]
		// 3	Name Error：名字错误，该值只对权威应答有意义，它表示请求的域名不存在。[RFC1035]
		// 4	Not Implemented：未实现，域名服务器不支持该查询类型。[RFC1035]
		// 5	Refused：拒绝服务，服务器由于设置的策略拒绝给出应答。比如，服务器不希望对个请求者给出应答时可以使用此响应码。[RFC1035]

		uint16_t _qdcount;//16bit, question 问题数。
		uint16_t _ancount;//16bit, answer 应答数
		uint16_t _nscount;//16bit, Authority 权威资源数
		uint16_t _arcount;//16bit, additional 附加资源数

		dnsPkgHead() {
			reset();
		}
		void parse(const uint8_t* pkg)
		{
			_id = pkg[0];
			_id = (_id << 8) | pkg[1];

			uint16_t uf = pkg[2];
			uf = (uf << 8) | pkg[3];

			_qr = uf >> 15;
			_optcode = (uf >> 11) & 0x0F;
			_aa = (uf >> 10) & 0x01;
			_tc = (uf >> 9) & 0x01;
			_rd = (uf >> 8) & 0x01;

			_ra = (uf >> 7) & 0x01;
			_res = (uf >> 4) & 0x07;
			_rcode = uf & 0x0F;

			_qdcount = pkg[4];
			_qdcount = (_qdcount << 8) | pkg[5];

			_ancount = pkg[6];
			_ancount = (_ancount << 8) | pkg[7];

			_nscount = pkg[8];
			_nscount = (_nscount << 8) | pkg[9];

			_arcount = pkg[10];
			_arcount = (_arcount << 8) | pkg[11];
		}

		void serialize(ec::stream& sout)
		{
			sout.setpos(0);
			sout < _id;
			uint16_t uf = (_qr << 15);

			uf |= (_optcode << 11);
			uf |= (_aa << 10);
			uf |= (_tc << 9);
			uf |= (_rd << 8);

			uf |= (_ra << 7);
			uf |= (_res << 4);
			uf |= _rcode;
			sout < uf;

			sout < _qdcount;
			sout < _ancount;
			sout < _nscount;
			sout < _arcount;
		}

		void reset() {
			_id = 0;
			_optcode = 0;
			_aa = 0;
			_tc = 0;
			_rd = 0;
			_ra = 0;
			_res = 0;
			_rcode = 0;
			_qdcount = 0;
			_ancount = 0;
			_nscount = 0;
			_arcount = 0;
		}
	};

	/**
	 * @brief dns tooltips
	 */
	class dnsTool
	{
	public:
		/**
		 * @brief 获取ipv4反向查找名
		 * @param paddr 网络序ipv4地址
		 * @param sout 输出
		 * 192.168.1.36 -> 36.1.168.192.in-addr.arpa
		 */
		template<class _STR>
		static void getPtrIpv4(const uint8_t* paddr, _STR& sout) {
			char s[64];
			int n = sprintf(s, "%u.%u.%u.%u.in-addr.arpa", paddr[3], paddr[2], paddr[1], paddr[0]);
			if (n > 0) {
				sout.append(s, n);
			}
		}

		/**
		 * @brief 获取ipv6的反向查找名
		 * @param paddr 网络序16字节
		 * @param sout 输出
		 * 4321:0:1:2:3:4:567:89ab -> b.a.9.8.7.6.5.0.4.0.0.0.3.0.0.0.2.0.0.0.1.0.0.0.0.0.0.0.1.2.3.4.ip6.arpa
		 */
		template<class _STR>
		static void getPtrIpv6(const uint8_t* paddr, _STR& sout) {
			uint8_t u;
			int i;
			for (i = 15; i >= 0; i--) {
				u = paddr[i] & 0x0F;
				if (u <= 9) {
					sout.push_back('0' + u);
				}
				else {
					sout.push_back('a' + u - 0x0a);
				}
				sout.push_back('.');
				u = paddr[i] >> 4;
				if (u <= 9) {
					sout.push_back('0' + u);
				}
				else {
					sout.push_back('a' + u - 0x0a);
				}
				sout.push_back('.');
			}
			sout.append("ip6.arpa");
		}

		/**
		 * @brief 解析指针lable
		 * @param npos 指针位置
		 * @param pkg 完整报文
		 * @param pkglen 完整报文长度
		 * @param sout 输出
		 * @param plog 日志
		 * @return 0：ok; -1: failed
		 */
		static int parsePtrLable(int npos, const uint8_t* pkg, int pkglen, dnsString& sout, ec::ilog* plog)
		{
			int nlen = npos, n = 0, nl;
			const uint8_t* p = pkg + nlen;
			while (*p && *p < 0x40) { //这点rfc1035文档没有说清楚，遇到下一个指针也算结束。
				nl = *p;
				if (nl + nlen >= pkglen) {
					if (plog) {
						plog->add(CLOG_DEFAULT_ERR, "parsePtrLable pos=%d failed, nl + nlen >= pkglen; nl=%d, nlen=%d, pkglen=%d",
							npos, nl, nlen, pkglen);
					}
					return -1;
				}
				if (n)
					sout.push_back('.');
				sout.append((const char*)p + 1, nl);
				nlen += nl + 1;
				p = pkg + nlen;
				++n;
			}
			return 0;
		}

		/**
		 * @brief 解析lable，不含单个0结束
		 * @param p 解析开始地址
		 * @param len 剩余长度
		 * @param s 输出
		 * @param pkg 完整报文
		 * @param pkglen 完整报文长度
		 * @param bend 回填 1表示结束
		 * @param plog 日志
		 * @return 返回解析的字节数
		 */
		static int parseLable(const uint8_t* p, int len, dnsString& s, const uint8_t* pkg, int pkglen, int& bend, ec::ilog* plog)
		{
			int nr = 0;
			bend = 0;
			if (p[0] < 0x40) { //简单直接lable
				if (p[0] >= len) {
					if (plog) {
						plog->add(CLOG_DEFAULT_ERR, "parseLable failed, p[0] > len, p[0]=%u, len=%d", p[0], len);
					}
					return -1;
				}
				s.append((const char*)p + 1, p[0]);
				nr = p[0] + 1;
			}
			else {//指针lable
				uint16_t npos = p[0] & 0x3f;
				npos = (npos << 8) | p[1];
				if ((int)npos >= pkglen) {
					if (plog) {
						plog->add(CLOG_DEFAULT_ERR, "parseLable failed, npos > pkglen, npos=%u, pkglen=%d", npos, pkglen);
					}
					return -1;
				}
				if (parsePtrLable((int)npos, pkg, pkglen, s, plog) < 0) {
					return -1;
				}
				nr = 2;
				bend = 1;
			}
			return nr;
		}

		/**
		 * @brief 解析一个域名到字符串对象
		 * @param p 解析开始地址
		 * @param len 剩余长度
		 * @param s 输出字符串
		 * @param pkg 原始报文，用于解析指针lable
		 * @param pkglen 原始报文长度
		 * @param plog 日志
		 * @return 返回解析的字节数,-1失败;
		 */
		static int parseName(const uint8_t* p, int len, dnsString& s, const uint8_t* pkg, int pkglen, ec::ilog* plog)
		{
			s.clear();
			int ndo = 0, nf = 0, n, nd = 0;
			if (len <= 0)
				return -1;
			while (p[0]) {
				if (nf) {
					s.push_back('.');
				}
				n = parseLable(p, len, s, pkg, pkglen, nd, plog);
				if (n < 0)
					return -1;
				p += n;
				len -= n;
				ndo += n;
				if (len <= 0)
					return -1;
				nf++;
				if (nd)
					return ndo;
			}
			return ndo + 1;
		}

		/**
		 * @brief 输出名称，标准模式，无指针
		 * @param s
		 * @param sout
		 * @return 返回0成功；-1失败
		 */
		static int serializeName(const char* s, ec::stream& sout)
		{
			if (!s || !*s)
				return -1;
			try {
				uint8_t n = 0;
				const char* pc = s;
				while (*s) {
					if (*s == '.' || *s == '@') { //输出一个label
						sout < n;
						sout.write(pc, n);
						++s;
						pc = s;
						n = 0;
					}
					else {
						++s;
						++n;
						if (n > 63)
							return -1;
					}
				}
				if (n) {
					sout < n;
					sout.write(pc, n);
				}
				sout < (uint8_t)0;//输出结束符
			}
			catch (...) {
				return -1;
			}
			return 0;
		}
	};
	/**
	 * @brief DNS报文question条目
	 */
	class dnsQuestion
	{
	public:
		dnsString _name;//多个lable组成，lable包括普通和压缩两种。"kipway.com"由两个lable组成
		uint16_t _qtype;// 16bit， https://www.iana.org/assignments/dns-parameters/dns-parameters.xhtml#dns-parameters-4
		//	A		1	主机地址[RFC1035]
		//	NS		2	域名服务器记录[RFC1035]
		//	CNAME	5	域名别名[RFC1035]
		//	SOA		6	权威记录起始[RFC1035]
		//	PTR		12	域名指针，用于IP解析域名[RFC1035]
		//	MX		15	邮件交换[RFC1035]
		//	TXT		16	文本记录[RFC1035]
		//	AAAA	28	IPV6主机地址[RFC3596]
		//	SRV		33	服务位置资源记录[RFC2782]
		//	DS		43	委托签发者，用于DNSSEC[RFC4034][RFC3658]
		//	RRSIG	46	用于DNSSEC[RFC4034][RFC3755]
		//	NSEC	47	用于DNSSEC[RFC4034][RFC3755]
		//	DNSKEY	48	用于DNSSEC[RFC4034][RFC3755]
		//  *		255 所有可能匹配的。

		uint16_t _qclass; //16bit， https://www.iana.org/assignments/dns-parameters/dns-parameters.xhtml#dns-parameters-2
		//	0	0x0000	Reserved[RFC6895]
		//	1	0x0001	Internet(IN)[RFC1035]
		//	2	0x0002	Unassigned
		//	3	0x0003	Chaos(CH)[D.Moon, "Chaosnet", A.I.Memo 628, Massachusetts Institute of Technology Artificial Intelligence Laboratory, June 1981.]
		//	4	0x0004	Hesiod(HS)[Dyer, S., and F.Hsu, "Hesiod", Project Athena Technical Plan - Name Service, April 1987.]
		//	5 - 253	0x0005 - 0x00FD	Unassigned
		//	254	0x00FE	QCLASS NONE[RFC2136]
		//	255	0x00FF	QCLASS * (ANY)[RFC1035]
		//	256 - 65279	0x0100 - 0xFEFF	Unassigned
		//	65280 - 65534	0xFF00 - 0xFFFE	Reserved for Private Use[RFC6895]
		//	65535	0xFFFF	Reserved[RFC6895]
	};

#ifdef _DNSUSE_ECMEM
	using dnsQuestions = ec::vector<dnsQuestion>;
#else
	using dnsQuestions = std::vector<dnsQuestion>;
#endif

	/**
	 * @brief SOA记录
	 */
	class dns_SoaRecord
	{
	public:
		dnsString _mname;//主源DNS
		dnsString _rname;//邮件地址
		uint32_t _serial;//序列号
		uint32_t _refresh;//刷新周期,单位秒
		uint32_t _retry;//重试倒计时,单位秒
		uint32_t _expire;//权威到期时间,单位秒
		uint32_t _mininum;//最小时间,单位秒

	public:
		int parse(const void* pd, int size, const uint8_t* pkg, int pkgsize, ec::ilog* plog)
		{
			const uint8_t* pu = (const uint8_t*)pd;

			int nlen = size;
			int n = dnsTool::parseName(pu, nlen, _mname, pkg, pkgsize, plog);
			if (n < 0) {
				if (plog)
					plog->add(CLOG_DEFAULT_ERR, "parseName failed @dns_SoaRecord::parse _mname");
				return -1;
			}
			pu += n;
			nlen -= n;

			n = dnsTool::parseName(pu, nlen, _rname, pkg, pkgsize, plog);
			if (n < 0) {
				if (plog)
					plog->add(CLOG_DEFAULT_ERR, "parseName failed @dns_SoaRecord::parse _rname");
				return -1;
			}
			pu += n;
			nlen -= n;
			if (nlen < 20)
				return -1;
			ec::stream ss((void*)pu, nlen);
			try {
				ss > _serial;
				ss > _refresh;
				ss > _retry;
				ss > _expire;
				ss > _mininum;
			}
			catch (...) {
				return -1;
			}
			return 0;
		}
		/**
		 * @brief 输出报文
		 * @param buf 输出缓冲区
		 * @param sizebuf 缓冲区大小, 不小于512
		 * @return 返回输出的字节数；-1失败。
		 */
		int serialize(void* buf, size_t sizebuf)
		{
			ec::stream ss(buf, sizebuf);
			if (dnsTool::serializeName(_mname.c_str(), ss) < 0)
				return -1;
			if (dnsTool::serializeName(_rname.c_str(), ss) < 0)
				return -1;
			try {
				ss < _serial;
				ss < _refresh;
				ss < _retry;
				ss < _expire;
				ss < _mininum;
			}
			catch (...) {
				return -1;
			}
			return (int)ss.getpos();
		}

		void logout(const char* smsg, ec::ilog* plog)
		{
			plog->add(CLOG_DEFAULT_DBG, "%s mname=%s, rname=%s, serial=%u, refresh=%u, retry=%u, expire=%u, mininum=%u",
				smsg, _mname.c_str(), _rname.c_str(), _serial, _refresh, _retry, _expire, _mininum
			);
		}
	};

	/**
	 * @brief dns资源记录
	 */
	class dnsResourceRecord
	{
	public:
		dnsString _name;//同 dnsQuestion:_name
		uint16_t _qtype;// 16bit，同 dnsQuestion:_name
		uint16_t _qclass; //16bit， 同 dnsQuestion:_class
		int32_t _ttl;// 生命期,单位秒.
		dnsBytes _data; //资源数据

		dns_SoaRecord _soa;
		dnsString _cname;
		void setSoa(const char* dnsname, const char* mname, const char* rname, int refresh)
		{
			_name = dnsname;
			_qtype = 6;
			_qclass = 1;
			_ttl = 600;
			_soa._mname = mname;
			_soa._rname = rname;
			_soa._mininum = 120;
			_soa._refresh = refresh;
			_soa._retry = 10;
			_soa._serial = 1;
			_soa._expire = 420;
			char stmp[200];
			int nl = _soa.serialize(stmp, sizeof(stmp));
			if (nl > 0)
				_data.assign((uint8_t*)stmp, (size_t)nl);
			else
				_data.clear();
		}
	};

#ifdef _DNSUSE_ECMEM
	using dnsResourceRecords = ec::vector<dnsResourceRecord>;
#else
	using dnsResourceRecords = std::vector<dnsResourceRecord>;
#endif

	/**
	 * @brief DNS报文
	 */
	class dnsPackage
	{
	public:
		dnsPkgHead _head; //报文头部
		dnsQuestions _question;//请求资源数组
		dnsResourceRecords _Answer;//应答资源数组
		dnsResourceRecords _Authority;//权威资源数组
		dnsResourceRecords _Additional;//附加资源数组
	public:
		/**
		 * @brief 解析报文
		 * @param pkg 报文,接收到的网络字节顺序的原始报文.
		 * @param size 报文字节数
		 * @return 0:OK; -1:failed
		 */
		int Parse(const void* pkg, int size, ec::ilog* plog)
		{
			if (size < 12) {
				return -1;
			}
			int nlen = size, n;
			const uint8_t* pu = (const uint8_t*)pkg;
			_head.parse(pu); //解析头部
			pu += 12;
			nlen -= 12;

			n = parseQustions(pu, nlen, (const uint8_t*)pkg, size, plog);//解析请求资源
			if (n < 0) {
				if (plog) {
					plog->add(CLOG_DEFAULT_ERR, "parseQustions failed.");
				}
				return -1;
			}
			pu += n;
			nlen -= n;

			n = parseResorceRecords(_head._ancount, pu, nlen, (const uint8_t*)pkg, size, _Answer, plog);
			if (n < 0) {
				if (plog) {
					plog->add(CLOG_DEFAULT_ERR, "parseResorceRecords Answer failed.");
				}
				return -1;
			}
			pu += n;
			nlen -= n;

			n = parseResorceRecords(_head._nscount, pu, nlen, (const uint8_t*)pkg, size, _Authority, plog);
			if (n < 0) {
				if (plog) {
					plog->add(CLOG_DEFAULT_ERR, "parseResorceRecords Authority failed.");
				}
				return -1;
			}
			pu += n;
			nlen -= n;

			n = parseResorceRecords(_head._arcount, pu, nlen, (const uint8_t*)pkg, size, _Additional, plog);
			if (n < 0) {
				if (plog) {
					plog->add(CLOG_DEFAULT_ERR, "parseResorceRecords Additional failed.");
				}
				return -1;
			}
			pu += n;
			nlen -= n;

			return 0;
		}

		/**
		 * @brief 输出报文
		 * @param buf 输出缓冲区
		 * @param sizebuf 缓冲区大小, 不小于512
		 * @return 返回输出的字节数；-1失败。
		 */
		int serialize(void* buf, size_t sizebuf)
		{
			ec::stream ss(buf, sizebuf);
			_head.serialize(ss);
			if (serializeQuestions(ss) < 0)
				return -1;
			if (serializeResourceRecords(ss) < 0)
				return -1;
			return (int)ss.getpos();
		}

		/**
		 * @brief 添加一个AAAA(ipv6)记录
		 * @param dnsName 域名,字符串,"kipway.com"
		 * @param pin6_addr 网络顺序的128bit地址,inet_pton转后的地址 in6_addr
		 */
		void addRecordAAAA(const char* dnsName, const void* pin6_addr, bool additional = false)
		{
			dnsResourceRecord rd;
			rd._name = dnsName;
			rd._qtype = 28;//AAAA	28	IPV6主机地址[RFC3596]
			rd._qclass = 1;//Internet 
			rd._data.append((uint8_t*)pin6_addr, 16);
			rd._ttl = EC_DNS_TTL;
			if (additional) {
				++_head._arcount;
				_Additional.push_back(std::move(rd));
			}
			else {
				++_head._ancount;
				_Answer.push_back(std::move(rd));
			}
		}

		/**
		 * @brief 添加一个A记录(ipv4)
		 * @param dnsName 域名
		 * @param pin4_addr 网络字节序4字节地址
		 * @param additional 是否添加到附加，默认添加到Answer
		 */
		void addRecordA(const char* dnsName, const void* pin4_addr, bool additional = false)
		{
			dnsResourceRecord rd;
			rd._name = dnsName;
			rd._qtype = 1;//A	IPV4主机地址[RFC3596]
			rd._qclass = 1;//Internet 
			rd._data.append((uint8_t*)pin4_addr, 4);
			rd._ttl = EC_DNS_TTL;
			if (additional) {
				++_head._arcount;
				_Additional.push_back(std::move(rd));
			}
			else {
				++_head._ancount;
				_Answer.push_back(std::move(rd));
			}
		}

		/**
		 * @brief 添加一个反向查询记录到Answer
		 * @param qname 查询名
		 * @param dnsName 域名
		 */
		void addRecordPtr(const char* qname, const char* dnsName)
		{
			dnsResourceRecord rd;
			rd._name = qname;
			rd._qtype = 12;//PTR
			rd._qclass = 1;//Internet 

			uint8_t tmp[128];
			ec::stream ss(tmp, sizeof(tmp));
			int n = dnsTool::serializeName(dnsName, ss);
			if (0 == n)
				rd._data.append((uint8_t*)tmp, ss.getpos());
			rd._ttl = 600;

			++_head._ancount;
			_Answer.push_back(std::move(rd));
		}

		void addRecordNS(const char* qname, const char* dnsName)
		{
			dnsResourceRecord rd;
			rd._name = qname;
			rd._qtype = 2;//NS
			rd._qclass = 1;//Internet 

			uint8_t tmp[128];
			ec::stream ss(tmp, sizeof(tmp));
			int n = dnsTool::serializeName(dnsName, ss);
			if (0 == n)
				rd._data.append((uint8_t*)tmp, ss.getpos());
			rd._ttl = 600;
			rd._cname = dnsName;
			++_head._ancount;
			_Answer.push_back(std::move(rd));
		}

		void addRecordCNAME(const char* qname, const char* cName)
		{
			dnsResourceRecord rd;
			rd._name = qname;
			rd._qtype = 5;//CNAME
			rd._qclass = 1;//Internet 

			uint8_t tmp[128];
			ec::stream ss(tmp, sizeof(tmp));
			int n = dnsTool::serializeName(cName, ss);
			if (0 == n)
				rd._data.append((uint8_t*)tmp, ss.getpos());
			rd._ttl = 600;
			rd._cname = cName;
			++_head._ancount;
			_Answer.push_back(std::move(rd));
		}

		void addRecordTXT(const char* qname, const char* stxt)
		{
			if (!stxt || !*stxt)
				return;
			dnsResourceRecord rd;
			rd._name = qname;
			rd._qtype = 16;//TXT
			rd._qclass = 1;//Internet 
			rd._data.append((uint8_t*)stxt, strlen(stxt));
			rd._ttl = 600;
			rd._cname = stxt;
			++_head._ancount;
			_Answer.push_back(std::move(rd));
		}

		void addSoa(const char* dnsName, const void* pdata, int datalen)
		{
			dnsResourceRecord rd;
			rd._name = dnsName;
			rd._qtype = 1;//A	IPV4主机地址[RFC3596]
			rd._qclass = 1;//Internet 
			rd._data.append((uint8_t*)pdata, datalen);
			rd._ttl = 600;
			++_head._ancount;
			_Answer.push_back(std::move(rd));
		}

		/**
		 * @brief 添加一个HTTPS记录(65)
		 * @param qName 查询名
		 * @param Priority 优先级
		 * @param additional 是否添加到附加，默认添加到Answer
		 */
		void addRecordHTTPS(const char* qName, uint16_t Priority, const char* TargetName, uint16_t port, bool additional = false)
		{
			dnsResourceRecord rd;
			rd._name = qName;
			rd._qtype = 65;//65	IPV4主机地址[RFC9460]
			rd._qclass = 1;//Internet 
			rd._ttl = 600;

			rd._data.push_back((uint8_t)(Priority >> 8));
			rd._data.push_back((uint8_t)(Priority & 0xFF));

			uint8_t tmp[128];
			if (!TargetName || !*TargetName || strlen(TargetName) > sizeof(tmp) / 2) {
				rd._data.push_back((uint8_t)0);
			}
			else {
				ec::stream ss(tmp, sizeof(tmp));
				int n = dnsTool::serializeName(TargetName, ss);
				if (0 == n) {
					rd._data.append((uint8_t*)tmp, ss.getpos());
				}
				else {
					rd._data.push_back((uint8_t)0);
				}
			}
			if (port != 443) {
				rd._data.push_back((uint8_t)0);
				rd._data.push_back((uint8_t)3); // key 3 port

				rd._data.push_back((uint8_t)0);
				rd._data.push_back((uint8_t)2); // length 2 

				rd._data.push_back((uint8_t)(port >> 8));
				rd._data.push_back((uint8_t)(port & 0xFF)); // port value
			}
			if (additional) {
				++_head._arcount;
				_Additional.push_back(std::move(rd));
			}
			else {
				++_head._ancount;
				_Answer.push_back(std::move(rd));
			}
		}

		void InitResponse(int errcode, int aa)
		{
			_head._qr = 1;
			_head._aa = (uint8_t)aa;
			_head._ra = 0; //不支持递归
			_head._tc = 0;
			_head._res = 0;
			_head._rcode = (uint8_t)errcode;
			_head._ancount = 0;
			_head._nscount = 0;
			_head._arcount = 0;

			_Answer.clear();
			_Authority.clear();
			_Additional.clear();
		}

		void logout(const char* sdes, ec::ilog* plog)
		{
			if (!plog)
				return;
			ec::string slog;
			slog.format("%s id=%u, qr=%u, optcode=%u, aa=%u, ra=%d, rd=%u, tc=%u, rcode=%u,\n\tqcount=%u, ancount=%u, nscount=%u, arcount=%u",
				sdes, _head._id,
				_head._qr, _head._optcode, _head._aa, _head._ra, _head._rd, _head._tc, _head._rcode,
				_head._qdcount, _head._ancount, _head._nscount, _head._arcount);
			for (auto& i : _question) {
				slog.appendformat("\n\tQuestion: %s , qtype=%u, qclass=%u", i._name.c_str(), i._qtype, i._qclass);
			}
			for (auto& i : _Answer) {
				slog.appendformat("\n\tAnswer: %s , qtype=%u, qclass=%u, ttl=%d, datasize=%zu",
					i._name.c_str(), i._qtype, i._qclass, i._ttl, i._data.size());
				if (i._qtype == 1 && 4 == i._data.size()) {
					slog.appendformat(",ipv4=%u.%u.%u.%u", i._data.at(0), i._data.at(1), i._data.at(2), i._data.at(3));
				}
				else if (i._qtype == 28 && 16 == i._data.size()) {
					slog.appendformat(",ipv6=");
					char sip6[64];
					if (!inet_ntop(AF_INET6, i._data.data(), sip6, sizeof(sip6)))
						sip6[0] = 0;
					slog.append(sip6);
				}
				else if (2 == i._qtype) { // NS
					slog.appendformat(",DNS=%s", i._cname.c_str());
				}
				else if (5 == i._qtype) { // CNAME
					slog.appendformat(",CNAME=%s", i._cname.c_str());
				}
				else if (16 == i._qtype) { // TXT
					slog.append(",TXT=").append(i._data.data(), i._data.size());
				}
				else if (65 == i._qtype) {
					if (!i._cname.empty()) {
						slog.append(",HTTPS=");
						slog.append(i._cname.c_str(), i._cname.size());
					}
				}
			}
			for (auto& i : _Authority) {
				slog.appendformat("\n\tAuthority: %s , qtype=%u, qclass=%u, ttl=%d, datasize=%zu",
					i._name.c_str(), i._qtype, i._qclass, i._ttl, i._data.size());
				if (6 == i._qtype) {
					slog.appendformat("\n\t\tsoa: mname= %s , rname= %s, serial=%u, refresh=%u, retry=%u, expire=%u, mininum=%u",
						i._soa._mname.c_str(), i._soa._rname.c_str(),
						i._soa._serial, i._soa._refresh, i._soa._retry, i._soa._expire, i._soa._mininum);
				}
			}
			for (auto& i : _Additional) {
				slog.appendformat("\n\tAdditional: %s , qtype=%u, qclass=%u, ttl=%d, datasize=%zu",
					i._name.c_str(), i._qtype, i._qclass, i._ttl, i._data.size());
				if (65 == i._qtype) {
					if (!i._cname.empty()) {
						slog.append(",HTTPS=");
						slog.append(i._cname.c_str(), i._cname.size());
					}
				}
				else if (i._qtype == 28 && 16 == i._data.size()) {
					slog.appendformat(",ipv6=");
					char sip6[64];
					if (!inet_ntop(AF_INET6, i._data.data(), sip6, sizeof(sip6)))
						sip6[0] = 0;
					slog.append(sip6);
				}
				else if (2 == i._qtype) { // NS
					slog.appendformat(",DNS=%s", i._cname.c_str());
				}
				else if (5 == i._qtype) { // CNAME
					slog.appendformat(",CNAME=%s", i._cname.c_str());
				}
				else if (16 == i._qtype) { // TXT
					slog.append(",TXT=").append(i._data.data(), i._data.size());
				}
			}
			plog->add(CLOG_DEFAULT_DBG, "%s", slog.c_str());
		}

	protected:
		/**
		 * @brief 解析请求问题到_question
		 * @param pkg 完整报文
		 * @param size 完整报文字节数
		 * @return 返回解析的字节数,用于计算下一个字段的解析位置。-1标识错误
		 */
		int parseQustions(const uint8_t* pu, int nlen, const uint8_t* pkg, int pkgsize, ec::ilog* plog)
		{
			int nq = 0, ndo = 0, n;
			dnsQuestion qd;
			_question.clear();
			while (nq < _head._qdcount) {
				n = dnsTool::parseName(pu, nlen, qd._name, pkg, pkgsize, plog);
				if (n < 0)
					return -1;
				ndo += n;
				pu += n;
				nlen -= n;

				if (nlen < 2)
					return -1;
				qd._qtype = pu[0];
				qd._qtype = (qd._qtype << 8) | pu[1];
				ndo += 2;
				pu += 2;
				nlen -= 2;

				if (nlen < 2)
					return -1;
				qd._qclass = pu[0];
				qd._qclass = (qd._qclass << 8) | pu[1];
				ndo += 2;
				pu += 2;
				nlen -= 2;

				_question.push_back(std::move(qd));
				++nq;
			}
			return ndo;
		}

		/**
		 * @brief 输出资源请求。
		 * @param sout
		 * @return 0：OK；-1：failed.
		 */
		int serializeQuestions(ec::stream& sout)
		{
			for (auto& i : _question) {
				if (dnsTool::serializeName(i._name.c_str(), sout) < 0)
					return -1;
				try {
					sout < i._qtype;
					sout < i._qclass;
				}
				catch (...) {
					return -1;
				}
			}
			return 0;
		}

		int parseHttps(dnsResourceRecord& rd, const uint8_t* pkg, int pkgsize, ec::ilog* plog) //解析rd._data 到rd._cname
		{
			uint16_t Priority = rd._data[0];
			Priority = (Priority << 8) + rd._data[1];
			char stmp[80];
			sprintf(stmp, "%u ", Priority);
			rd._cname = stmp;
			dnsString tagname;
			int n = dnsTool::parseName(rd._data.data() + 2, (int)rd._data.size() - 2, tagname, pkg, pkgsize, plog);
			if (n > 0) {
				if (tagname.empty()) {
					rd._cname.push_back('.');
				}
				else {
					rd._cname.append(tagname.c_str(), tagname.size());
				}
				int palen = (int)rd._data.size() - 2 - n;
				if (palen > 4) {
					const uint8_t* pa = rd._data.data() + 2 + n;
					char st[80];
					while (palen > 4) {
						uint16_t ukey = pa[0], ulen=0;
						ukey = (ukey << 8) + pa[1];
						if (ukey == 3) { //port
							if (palen < 6) {
								return -1;
							}
							ulen = 2;
							uint16_t uport = pa[4];
							uport = (uport << 8) + pa[5];
							
							sprintf(st, " port=%u", uport);
							rd._cname.append(st);
							pa += (4 + ulen);
							palen -= (4 + ulen);
						}
						else {
							ulen = pa[2];
							ulen = (ulen << 8) + pa[3];

							sprintf(st, " key=%u,vlen%u", ukey, ulen);
							rd._cname.append(st);
							pa += (4 + ulen);
							palen -= (4 + ulen);
						}
					}
				}
				return 0;
			}
			return -1;
		}

		/**
		 * @brief 解析资源记录题到_question
		 * @param rdcount 资源个数
		 * @param p 解析开始位置
		 * @param len 到报文尾部的字节数
		 * @param pkg 完整报文
		 * @param size 完整报文字节数
		 * @return 返回解析的字节数,用于计算下一个字段的解析位置。-1标识错误
		 */
		int parseResorceRecords(int rdcount, const uint8_t* pu, int nlen, const uint8_t* pkg, int pkgsize, dnsResourceRecords& ro, ec::ilog* plog)
		{
			int rdlen = 0;
			int nrd = 0, ndo = 0, n;

			ro.clear();
			while (nrd < rdcount) {
				dnsResourceRecord rd;
				n = dnsTool::parseName(pu, nlen, rd._name, pkg, pkgsize, plog);
				if (n < 0)
					return -1;
				ndo += n;
				pu += n;
				nlen -= n;

				if (nlen < 2) {
					if (plog) {
						plog->add(CLOG_DEFAULT_ERR, "parseResorceRecords failed, no more bytes @qtype");
					}
					return -1;
				}
				rd._qtype = pu[0];
				rd._qtype = (rd._qtype << 8) | pu[1];
				ndo += 2;
				pu += 2;
				nlen -= 2;

				if (nlen < 2) {
					if (plog) {
						plog->add(CLOG_DEFAULT_ERR, "parseResorceRecords failed, no more bytes @qclass");
					}
					return -1;
				}
				rd._qclass = pu[0];
				rd._qclass = (rd._qclass << 8) | pu[1];
				ndo += 2;
				pu += 2;
				nlen -= 2;

				if (nlen < 4) {
					if (plog) {
						plog->add(CLOG_DEFAULT_ERR, "parseResorceRecords failed, no more bytes @ttl");
					}
					return -1;
				}
				rd._ttl = pu[0];
				rd._ttl = (rd._ttl << 8) | pu[1];
				rd._ttl = (rd._ttl << 8) | pu[2];
				rd._ttl = (rd._ttl << 8) | pu[3];
				ndo += 4;
				pu += 4;
				nlen -= 4;

				if (nlen < 2) {
					if (plog) {
						plog->add(CLOG_DEFAULT_ERR, "parseResorceRecords failed, no more bytes @rdlen");
					}
					return -1;
				}
				rdlen = pu[0];
				rdlen = (rdlen << 8) + pu[1];
				ndo += 2;
				pu += 2;
				nlen -= 2;

				if (nlen < (int)rdlen) {
					if (plog) {
						plog->add(CLOG_DEFAULT_ERR, "parseResorceRecords failed, no more bytes @rddata");
					}
					return -1;
				}

				rd._data.append(pu, rdlen);
				ndo += rdlen;
				pu += rdlen;
				nlen -= rdlen;

				if (6 == rd._qtype && rdlen > 0) {
					rd._soa.parse(rd._data.data(), (int)rd._data.size(), pkg, pkgsize, plog);
				}
				else if ((2 == rd._qtype || 5 == rd._qtype) && rdlen > 0) {
					dnsTool::parseName(rd._data.data(), (int)rd._data.size(), rd._cname, pkg, pkgsize, plog);
				}
				else if (65 == rd._qtype && rd._data.size() > 4) {//解析到 cName用于logout
					parseHttps(rd, pkg, pkgsize, plog);
				}
				ro.push_back(std::move(rd));
				++nrd;
			}
			return ndo;
		}

		/**
		 * @brief 输出剩下的三个资源记录集
		 * @param sout
		 * @return
		 */
		int serializeResourceRecords(ec::stream& sout)
		{
			for (auto& i : _Answer) {
				if (dnsTool::serializeName(i._name.c_str(), sout) < 0)
					return -1;
				try {
					sout < i._qtype;
					sout < i._qclass;
					sout < (uint32_t)i._ttl;
					sout < (uint16_t)i._data.size();
					sout.write(i._data.data(), i._data.size());
				}
				catch (...) {
					return -1;
				}
			}
			for (auto& i : _Authority) {
				if (dnsTool::serializeName(i._name.c_str(), sout) < 0)
					return -1;
				try {
					sout < i._qtype;
					sout < i._qclass;
					sout < (uint32_t)i._ttl;
					sout < (uint16_t)i._data.size();
					sout.write(i._data.data(), i._data.size());
				}
				catch (...) {
					return -1;
				}
			}
			for (auto& i : _Additional) {
				if (dnsTool::serializeName(i._name.c_str(), sout) < 0)
					return -1;
				try {
					sout < i._qtype;
					sout < i._qclass;
					sout < (uint32_t)i._ttl;
					sout < (uint16_t)i._data.size();
					sout.write(i._data.data(), i._data.size());
				}
				catch (...) {
					return -1;
				}
			}
			return 0;
		}
	};
}
