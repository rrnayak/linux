/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c) 2019, The Linux Foundation. All rights reserved. */

#undef TRACE_SYSTEM
#define TRACE_SYSTEM io

#if !defined(CONFIG_TRACING_EVENTS_IO)
#define NOTRACE
#endif

#undef TRACE_INCLUDE_FILE
#define TRACE_INCLUDE_FILE io-trace

#undef TRACE_INCLUDE_PATH
#define TRACE_INCLUDE_PATH asm-generic

#if !defined(_TRACE_IO_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_IO_H

#include <linux/tracepoint.h>

/*
 * Tracepoint for generic IO read/write, i.e., __raw_{read,write}{b,l,w,q}()
 */
DECLARE_EVENT_CLASS(io_trace_class,

	TP_PROTO(const char *type, int cpu, u64 ts, void *addr,
		 unsigned long ret_ip),

	TP_ARGS(type, cpu, ts, addr, ret_ip),

	TP_STRUCT__entry(
		__string(	type,		type	)
		__field(	int,		cpu	)
		__field(	u64,		ts	)
		__field(	void *,		addr	)
		__field(	unsigned long,	ret_ip	)
	),

	TP_fast_assign(
		__assign_str(type, type);
		__entry->cpu	= cpu;
		__entry->ts	= ts;
		__entry->addr	= addr;
		__entry->ret_ip	= ret_ip;
	),

	TP_printk("type=%s cpu=%d ts:%llu data=0x%lx caller=%pS",
		  __get_str(type), __entry->cpu, __entry->ts,
		  (unsigned long)__entry->addr, (void *)__entry->ret_ip)
);

DEFINE_EVENT(io_trace_class, io_read,

	TP_PROTO(const char *type, int cpu, u64 ts, void *addr,
		 unsigned long ret_ip),

	TP_ARGS(type, cpu, ts, addr, ret_ip)
);

DEFINE_EVENT(io_trace_class, io_write,

	TP_PROTO(const char *type, int cpu, u64 ts, void *addr,
		 unsigned long ret_ip),

	TP_ARGS(type, cpu, ts, addr, ret_ip)
);

#endif /* _TRACE_IO_H */

/* This part must be outside protection */
#include <trace/define_trace.h>
