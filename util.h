/**
 * Copyright (c) 2018 Wei-Lun Hsu. All Rights Reserved.
 */
/** @file util.h
 *
 * @author Wei-Lun Hsu
 * @version 0.1
 * @date 2018/02/03
 * @license
 * @description
 */

#ifndef __util_H_w9qZ7Kx8_lP6r_HWr9_sNur_uNhO5dWpEwc5__
#define __util_H_w9qZ7Kx8_lP6r_HWr9_sNur_uNhO5dWpEwc5__

#ifdef __cplusplus
extern "C" {
#endif


//=============================================================================
//                  Constant Definition
//=============================================================================

//=============================================================================
//                  Macro Definition
//=============================================================================
#define PUSH_STRING(pStr_buf, layer, str_format, ...)                            \
    do{ int  k;                                                                  \
        for(k=0;k<layer;k++) sprintf(&pStr_buf[strlen(pStr_buf)], "    ");       \
        sprintf(&pStr_buf[strlen(pStr_buf)], str_format, __VA_ARGS__);           \
    }while(0)

#define FLUSH_STRING(pFileIO, pStr_buf)             \
    do{ fprintf(pFileIO, "%s", pStr_buf);           \
        *(pStr_buf) = '\0';                         \
    }while(0)

//=============================================================================
//                  Structure Definition
//=============================================================================

//=============================================================================
//                  Global Data Definition
//=============================================================================

//=============================================================================
//                  Private Function Definition
//=============================================================================

//=============================================================================
//                  Public Function Definition
//=============================================================================

#ifdef __cplusplus
}
#endif

#endif
