/**
 * Copyright (c) 2018 Wei-Lun Hsu. All Rights Reserved.
 */
/** @file main.c
 *
 * @author Wei-Lun Hsu
 * @version 0.1
 * @date 2018/02/01
 * @license
 * @description
 */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "iniparser.h"
#include "crc32.h"
#include "partial_read.h"
#include "regex.h"
#include "util.h"
//=============================================================================
//                  Constant Definition
//=============================================================================
#define LIMIT_YEAR                  2020
#define LIMIT_MONTH                 2

#define MAX_MAP_FILES               5

#define MAX_BUFFER_SIZE             (2 << 20)
#define MAX_STR_LEN                 256

#define DECLARING_MESSAGE           "Automatically generated file; DO NOT EDIT."
//=============================================================================
//                  Macro Definition
//=============================================================================
#define err_msg(str, args...)           do{ fprintf(stderr, "%s[%u] " str, __func__, __LINE__, ##args); while(1);}while(0)

#define dbg_msg(str, args...)           fprintf(stderr, "%s[%u] " str, __func__, __LINE__, ##args);

#define REGEX_MATCH_EXTRACT(pBuf, pLine_str, regmatch_info, match_idx)     \
    strncpy(pBuf, &pLine_str[regmatch_info[match_idx].rm_so], regmatch_info[match_idx].rm_eo - regmatch_info[match_idx].rm_so)

#define BIG_ENDIAN(x)       ((((x) & 0xff) << 24) | (((x) & 0xff00) << 8) | (((x) & 0xff0000) >> 8) | (((x) & 0xff000000) >> 24))
//=============================================================================
//                  Structure Definition
//=============================================================================
typedef struct obj_info
{
    struct obj_info     *next;

    char        obj_name[64];
    uint32_t    code_size;
    uint32_t    inc_data_size;
    uint32_t    ro_data_size;
    uint32_t    rw_data_size;
    uint32_t    zi_data_size;
    uint32_t    debug_size;

} obj_info_t;

//=============================================================================
//                  Global Data Definition
//=============================================================================
static obj_info_t   *g_pObj_first = 0;
//=============================================================================
//                  Private Function Definition
//=============================================================================
static int
_create_reader(
    partial_read_t  *pHReader,
    const char      *pPath)
{
    int     rval = 0;

    do {
        if( !pPath )
        {
            err_msg("%s", "null path \n");
            rval = -1;
            break;
        }

        if( !(pHReader->fp = fopen(pPath, "rb")) )
        {
            err_msg("open '%s' fail \n", pPath);
            rval = -1;
            break;
        }

        pHReader->buf_size = MAX_BUFFER_SIZE;
        if( !(pHReader->pBuf = malloc(pHReader->buf_size)) )
        {
            err_msg("malloc '%ld' fail \n", pHReader->buf_size);
            rval = -1;
            break;
        }

        pHReader->pCur          = pHReader->pBuf;
        pHReader->pEnd          = pHReader->pCur;

        fseek(pHReader->fp, 0l, SEEK_END);
        pHReader->file_size = ftell(pHReader->fp);
        fseek(pHReader->fp, 0l, SEEK_SET);

        pHReader->file_remain = pHReader->file_size;

    } while(0);

    if( rval )
    {
        if( pHReader->fp )      fclose(pHReader->fp);
        pHReader->fp = 0;

        if( pHReader->pBuf )    free(pHReader->pBuf);
        pHReader->pBuf = 0;
    }

    return rval;
}

static int
_destroy_reader(
    partial_read_t  *pHReader)
{
    int         rval = 0;

    if( pHReader->fp )      fclose(pHReader->fp);
    if( pHReader->pBuf )    free(pHReader->pBuf);

    memset(pHReader, 0x0, sizeof(partial_read_t));
    return rval;
}

static int
_post_read(unsigned char *pBuf, int buf_size)
{
    int     i;
    for(i = 0; i < buf_size; ++i)
    {
        if( pBuf[i] == '\n' || pBuf[i] == '\r' )
            pBuf[i] = '\0';
    }
    return 0;
}


static int
_parse_map_file(
    partial_read_t  *pHReader,
    obj_info_t      **ppObj_info)
{
    int         rval = 0;
    uint32_t    bFind_start_tag = 0;
    regex_t     hRegex_obj_info = {0};

    regcomp(&hRegex_obj_info, "\\s+([0-9]+)\\s+([0-9]+)\\s+([0-9]+)\\s+([0-9]+)\\s+([0-9]+)\\s+([0-9]+)\\s+([A-Za-z0-9_-]+).o$", REG_EXTENDED);
    if( rval )
    {
        char    msgbuf[MAX_STR_LEN] = {0};
        regerror(rval, &hRegex_obj_info, msgbuf, sizeof(msgbuf));
        printf("%s\n", msgbuf);
        return -1;
    }


    partial_read__full_buf(pHReader, _post_read);
    while( pHReader->pCur < pHReader->pEnd )
    {
        if( partial_read__full_buf(pHReader, _post_read) )
        {
            break;
        }

        {   // start parsing a line
            char            *pAct_str = 0;
            char            str_buf[MAX_STR_LEN] = {0};
            size_t          nmatch = 8;
            regmatch_t      match_info[8] = {{0}};

            pAct_str = (char*)pHReader->pCur;

            pHReader->pCur += (strlen((char*)pHReader->pCur) + 1);

            char    *ptt = strstr(pAct_str, "Image component sizes");
            if( bFind_start_tag == 0 )
            {
                bFind_start_tag = (strstr(pAct_str, "Image component sizes")) ? 1 : 0;
                continue;
            }

            rval = regexec(&hRegex_obj_info, pAct_str, nmatch, match_info, 0);

            if( rval == REG_NOMATCH || rval )
                continue;

            {   // check match items
                obj_info_t      *pObj_info_cur = 0;

                if( !(pObj_info_cur = malloc(sizeof(obj_info_t))) )
                {
                    err_msg("malloc %s fail !\n", sizeof(obj_info_t));
                    break;
                }
                memset(pObj_info_cur, 0x0, sizeof(obj_info_t));

                memset(str_buf, 0x0, MAX_STR_LEN);
                if( match_info[1].rm_so != -1 )
                {
                    REGEX_MATCH_EXTRACT(str_buf, pAct_str, match_info, 1);
                    pObj_info_cur->code_size = strtoul(str_buf, NULL, 10);
                }

                memset(str_buf, 0x0, MAX_STR_LEN);
                if( match_info[2].rm_so != -1 )
                {
                    REGEX_MATCH_EXTRACT(str_buf, pAct_str, match_info, 2);
                    pObj_info_cur->inc_data_size = strtoul(str_buf, NULL, 10);
                }

                memset(str_buf, 0x0, MAX_STR_LEN);
                if( match_info[3].rm_so != -1 )
                {
                    REGEX_MATCH_EXTRACT(str_buf, pAct_str, match_info, 3);
                    pObj_info_cur->ro_data_size = strtoul(str_buf, NULL, 10);
                }

                memset(str_buf, 0x0, MAX_STR_LEN);
                if( match_info[4].rm_so != -1 )
                {
                    REGEX_MATCH_EXTRACT(str_buf, pAct_str, match_info, 4);
                    pObj_info_cur->rw_data_size = strtoul(str_buf, NULL, 10);
    }

                memset(str_buf, 0x0, MAX_STR_LEN);
                if( match_info[5].rm_so != -1 )
    {
                    REGEX_MATCH_EXTRACT(str_buf, pAct_str, match_info, 5);
                    pObj_info_cur->zi_data_size = strtoul(str_buf, NULL, 10);
}

                memset(str_buf, 0x0, MAX_STR_LEN);
                if( match_info[6].rm_so != -1 )
{
                    REGEX_MATCH_EXTRACT(str_buf, pAct_str, match_info, 6);
                    pObj_info_cur->debug_size = strtoul(str_buf, NULL, 10);
    }

                memset(str_buf, 0x0, MAX_STR_LEN);
                if( match_info[7].rm_so != -1 )
    {
                    REGEX_MATCH_EXTRACT(str_buf, pAct_str, match_info, 7);
                    snprintf(pObj_info_cur->obj_name, 64, "%s", str_buf);
    }

                if( *ppObj_info )
{
                    obj_info_t      *pObj_info_tmp = (obj_info_t*)(*ppObj_info);

                    while( pObj_info_tmp->next )
    {
                        pObj_info_tmp = pObj_info_tmp->next;
    }

                    pObj_info_tmp->next = pObj_info_cur;
    }
                else
    {
                    *ppObj_info = pObj_info_cur;
        }
    }

    }
}

    regfree(&hRegex_obj_info);

        return rval;
    }

//=============================================================================
//                  Public Function Definition
//=============================================================================
void usage(char *progm)
{
    fprintf(stderr, "Copyright (c) 2018~ Wei-Lun Hsu. All rights reserved.\n"
            "%s [ini file]\n",
            progm);
    exit(-1);
}

int main(int arc, char **argv)
{
    int                 rval = 0;
    int                 i;
    dictionary          *pIni = 0;
    int                 map_file_cnt = 0;
    partial_read_t      *pHReader = 0, hRead_map = {0};
    char                *pIni_path = 0;

    pHReader = &hRead_map;

    do {
        char        str_buf[MAX_STR_LEN] = {0};
        const char  *pPath = 0;

        {
            time_t      rawtime;
            struct tm   *timeinfo;
            time(&rawtime);

            timeinfo = localtime(&rawtime);

            if( timeinfo->tm_year + 1900 >= LIMIT_YEAR &&
                timeinfo->tm_mon + 1 >= LIMIT_MONTH )
                return 0;
        }

        pIni_path = argv[1];

        pIni = iniparser_load(pIni_path);
        if( pIni == NULL )
        {
            err_msg("cannot parse file: '%s'\n", pIni_path);
            rval = -1;
            break;
        }

        snprintf(str_buf, MAX_STR_LEN, "%s", "in_file:map_file");
            pPath = iniparser_getstring(pIni, str_buf, NULL);
            if( !pPath )
            {
                rval = -1;
                err_msg("no '%s' file !\n", str_buf);
                break;
            }

        if( (rval = _create_reader(pHReader , pPath)) )
                break;

        _parse_map_file(pHReader, &g_pObj_first);

        #if 0
            {
            FILE            *fout = 0;
            obj_info_t      *pObj_tmp = g_pObj_first;

            if( !(fout = fopen("dbg_out.txt", "w")) )
            {
                err_msg("open %s fail \n", "dbg_out.txt");
            }

            while( pObj_tmp )
            {
                obj_info_t  *pcur = pObj_tmp;

                pObj_tmp = pObj_tmp->next;

                fprintf(fout, "\t%d\t%d\t%d\t%d\t%d\t%d\t%s.o\n",
                       pcur->code_size, pcur->inc_data_size, pcur->ro_data_size, pcur->rw_data_size,
                       pcur->zi_data_size, pcur->debug_size, pcur->obj_name);
            }

            if( fout )      fclose(fout);
        }
        #endif


    } while(0);

    _destroy_reader(pHReader);

    return 0;
}

