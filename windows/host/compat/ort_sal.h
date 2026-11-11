// Las anotaciones SAL que usa la cabecera de ONNX Runtime y que mingw no define (no cambian el código)
#pragma once
#include <specstrings.h>
#ifndef _In_
#define _In_
#endif
#ifndef _In_z_
#define _In_z_
#endif
#ifndef _In_opt_
#define _In_opt_
#endif
#ifndef _In_opt_z_
#define _In_opt_z_
#endif
#ifndef _Out_
#define _Out_
#endif
#ifndef _Outptr_
#define _Outptr_
#endif
#ifndef _Out_opt_
#define _Out_opt_
#endif
#ifndef _Inout_
#define _Inout_
#endif
#ifndef _Inout_opt_
#define _Inout_opt_
#endif
#ifndef _Frees_ptr_opt_
#define _Frees_ptr_opt_
#endif
#ifndef _Ret_maybenull_
#define _Ret_maybenull_
#endif
#ifndef _Ret_notnull_
#define _Ret_notnull_
#endif
#ifndef _Check_return_
#define _Check_return_
#endif
#ifndef _Outptr_result_maybenull_
#define _Outptr_result_maybenull_
#endif
#ifndef _In_reads_
#define _In_reads_(X)
#endif
#ifndef _Inout_updates_
#define _Inout_updates_(X)
#endif
#ifndef _Out_writes_
#define _Out_writes_(X)
#endif
#ifndef _Inout_updates_all_
#define _Inout_updates_all_(X)
#endif
#ifndef _Out_writes_bytes_all_
#define _Out_writes_bytes_all_(X)
#endif
#ifndef _Out_writes_all_
#define _Out_writes_all_(X)
#endif
#ifndef _Success_
#define _Success_(X)
#endif
#ifndef _Outptr_result_buffer_maybenull_
#define _Outptr_result_buffer_maybenull_(X)
#endif
