// WebView2.h la necesita y mingw-w64 no la trae: es solo este tipo (el mismo que en el SDK de Windows)
#pragma once
#ifndef __EventRegistrationToken_DEFINED
#define __EventRegistrationToken_DEFINED
typedef struct EventRegistrationToken { __int64 value; } EventRegistrationToken;
#endif
