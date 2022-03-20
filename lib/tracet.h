/* -*- C -*- */
/*
 * Copyright (c) 2012-2022 Seagate Technology LLC and/or its Affiliates
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * For any questions about this software or licensing,
 * please email opensource@seagate.com or cortx-questions@seagate.com.
 *
 */


#pragma once

#ifndef __MOTR_LIB_TRACET_H__
#define __MOTR_LIB_TRACET_H__

#include "lib/trace.h"

#include "fid/fid.h"
#include "lib/ext.h"
#include "be/tx_credit.h"
#include "be/log.h"
#include "lib/types.h"

/*
 * Type-aware logging.
 */

#define M0_LOGT(level, msg, ...)					\
	M0_LOG(level, msg M0_CAT(M0_LFMT_,				\
				 M0_COUNT_PARAMS(__VA_ARGS__))(__VA_ARGS__), \
	       M0_CAT(M0_LARG_, M0_COUNT_PARAMS(__VA_ARGS__))(__VA_ARGS__))

#define M0_LFMT_0(...) ""
#define M0_LFMT_1(v, ...) #v " = " M0_LFMT(v)
#define M0_LFMT_2(v, ...) #v " = " M0_LFMT(v) ", " M0_LFMT_1(__VA_ARGS__)
#define M0_LFMT_3(v, ...) #v " = " M0_LFMT(v) ", " M0_LFMT_2(__VA_ARGS__)
#define M0_LFMT_4(v, ...) #v " = " M0_LFMT(v) ", " M0_LFMT_3(__VA_ARGS__)
#define M0_LFMT_5(v, ...) #v " = " M0_LFMT(v) ", " M0_LFMT_4(__VA_ARGS__)
#define M0_LFMT_6(v, ...) #v " = " M0_LFMT(v) ", " M0_LFMT_5(__VA_ARGS__)
#define M0_LFMT_7(v, ...) #v " = " M0_LFMT(v) ", " M0_LFMT_6(__VA_ARGS__)
#define M0_LFMT_8(v, ...) #v " = " M0_LFMT(v) ", " M0_LFMT_7(__VA_ARGS__)
#define M0_LFMT_9(v, ...) #v " = " M0_LFMT(v) ", " M0_LFMT_8(__VA_ARGS__)

#define M0_LARG_0(...)
#define M0_LARG_1(v, ...) M0_LARG(v), M0_LARG_0(__VA_ARGS__)
#define M0_LARG_2(v, ...) M0_LARG(v), M0_LARG_1(__VA_ARGS__)
#define M0_LARG_3(v, ...) M0_LARG(v), M0_LARG_2(__VA_ARGS__)
#define M0_LARG_4(v, ...) M0_LARG(v), M0_LARG_3(__VA_ARGS__)
#define M0_LARG_5(v, ...) M0_LARG(v), M0_LARG_4(__VA_ARGS__)
#define M0_LARG_6(v, ...) M0_LARG(v), M0_LARG_5(__VA_ARGS__)
#define M0_LARG_7(v, ...) M0_LARG(v), M0_LARG_6(__VA_ARGS__)
#define M0_LARG_8(v, ...) M0_LARG(v), M0_LARG_7(__VA_ARGS__)
#define M0_LARG_9(v, ...) M0_LARG(v), M0_LARG_8(__VA_ARGS__)

#define M0_LFTM(x)					\
	_Generic(x,					\
		 bool     : "%s",			\
		 struct m0_fid * : FID_F,		\
		 struct m0_ext * : EXT_F,		\
		 struct m0_be_tx_credit * : BETXCR_F,	\
		 struct m0_be_log_record * : BLR_F,	\
		 struct m0_be_log * : BL_F,		\
		 struct m0_uint128 * : U128X_F,	\
		 uint64_t : "%"PRIx64,			\
		 uint32_t : "%"PRIx32,			\
		 uint16_t : "%"PRIx16,			\
		 uint8_t  : "%"PRIx8,			\
		 default : "%llx")

#define M0_LARG(x)							\
	({								\
		typeof(x) __x = (x);					\
		_Generic(x,						\
			 bool     : m0_bool_to_str(__x),		\
			 struct m0_fid * : FID_P(__x),			\
			 struct m0_ext * : EXT_P(__x),			\
			 struct m0_be_tx_credit * : BETXCR_P(__x),	\
			 struct m0_be_log_record * : BLR_P(__x),	\
			 struct m0_be_log * : BL_P(__x),		\
			 struct m0_uint128 * : U128X_P(__x),		\
			 uint64_t : __x,				\
			 uint32_t : __x,				\
			 uint16_t : __x,				\
			 uint8_t  : __x,				\
			 default  : (unsigned long long *)__x);	\
	})

/** @} end of trace group */
#endif /* __MOTR_LIB_TRACET_H__ */

/*
 *  Local variables:
 *  c-indentation-style: "K&R"
 *  c-basic-offset: 8
 *  tab-width: 8
 *  fill-column: 80
 *  scroll-step: 1
 *  End:
 */
