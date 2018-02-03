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
typedef struct rom_info
{
    struct rom_info     *next;

    char            rom_name[64];
    unsigned long   base_addr;
    unsigned long   rom_max_size;
    unsigned long   rom_size;

} rom_info_t;

typedef struct fw_info
{
    struct fw_info  *next;

    uint32_t        fw_uid;
    uint32_t        rom_cnt;
    rom_info_t      *pRom_info;

} fw_info_t;

typedef struct out_args
{
    union {
        struct {
            uint32_t        flash_start_addr;
            uint32_t        alignment;
            char            *pBin_dir;
        } rom_merge_list;

        struct {
            uint32_t        flash_mem_bass_addr;
            uint32_t        sram_mem_bass_addr;
            uint32_t        sram_mem_size;
        } app_bld_header;

        struct {
            uint32_t        fw_num;
            uint32_t        bEnable_AES;
            uint32_t        flash_start_addr;
            uint8_t         host_mark[8];
            uint32_t        uid_mark_0;
            uint32_t        uid_mark_1;
            uint32_t        md5[4];
            uint32_t        alignment;
        } fw_header;
    };

} out_args_t;
//=============================================================================
//                  Global Data Definition
//=============================================================================

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
    fw_info_t       *pFw_info)
{
    int         rval = 0;
    uint32_t    bFind_load_region = 0;
    regex_t     hRegex_load_region = {0};
    regex_t     hRegex_exe_region = {0};

    regcomp(&hRegex_load_region, "\\s+Load Region (\\w+) \\(Base: 0x([0-9a-fA-F]+), Size: 0x([0-9a-fA-F]+), Max: 0x([0-9a-fA-F]+),.*\\)$", REG_EXTENDED);
    if( rval )
    {
        char    msgbuf[MAX_STR_LEN] = {0};
        regerror(rval, &hRegex_load_region, msgbuf, sizeof(msgbuf));
        printf("%s\n", msgbuf);
        return -1;
    }

    regcomp(&hRegex_exe_region, "\\s+Execution Region (\\w+) \\(Base: 0x([0-9a-fA-F]+), Size: 0x([0-9a-fA-F]+), Max: 0x([0-9a-fA-F]+),.*\\)$", REG_EXTENDED);
    if( rval )
    {
        char    msgbuf[MAX_STR_LEN] = {0};
        regerror(rval, &hRegex_load_region, msgbuf, sizeof(msgbuf));
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
            size_t          nmatch = 5;
            regmatch_t      match_info[5] = {{0}};

            pAct_str = (char*)pHReader->pCur;

            pHReader->pCur += (strlen((char*)pHReader->pCur) + 1);

            rval = (bFind_load_region)
                 ? regexec(&hRegex_exe_region, pAct_str, nmatch, match_info, 0)
                 : regexec(&hRegex_load_region, pAct_str, nmatch, match_info, 0);

            if( rval == REG_NOMATCH || rval )
                continue;

            if( !bFind_load_region )
            {
                bFind_load_region = 1;
                continue;
            }

            bFind_load_region = 0;

            {   // check match items
                char            *pTmp = str_buf;
                // unsigned long   base_addr = 0l;
                // unsigned long   size = 0l;
                rom_info_t      *pCur_rom_info = 0;

                if( !(pCur_rom_info = malloc(sizeof(rom_info_t))) )
                {
                    rval = -1;
                    err_msg("malloc rom info (%d) fail !\n", sizeof(rom_info_t));
                    break;
                }
                memset(pCur_rom_info, 0x0, sizeof(rom_info_t));

                pFw_info->rom_cnt++;

                // extract info
                memset(pTmp, 0x0, MAX_STR_LEN);
                if( match_info[1].rm_so != -1 )
                {
                    REGEX_MATCH_EXTRACT(pTmp, pAct_str, match_info, 1);
                    snprintf(pCur_rom_info->rom_name, 64, "%s", pTmp);
                }

                memset(pTmp, 0x0, MAX_STR_LEN);
                if( match_info[2].rm_so != -1 )
                {
                    REGEX_MATCH_EXTRACT(pTmp, pAct_str, match_info, 2);
                    pCur_rom_info->base_addr = strtoul(pTmp, NULL, 16);
                }

                memset(pTmp, 0x0, MAX_STR_LEN);
                if( match_info[3].rm_so != -1 )
                {
                    REGEX_MATCH_EXTRACT(pTmp, pAct_str, match_info, 3);
                    pCur_rom_info->rom_size = strtoul(pTmp, NULL, 16);
                }

                memset(pTmp, 0x0, MAX_STR_LEN);
                if( match_info[4].rm_so != -1 )
                {
                    REGEX_MATCH_EXTRACT(pTmp, pAct_str, match_info, 4);
                    pCur_rom_info->rom_max_size = strtoul(pTmp, NULL, 16);
                }

                // add node to rom info
                if( !pFw_info->pRom_info )
                    pFw_info->pRom_info = pCur_rom_info;
                else
                {
                    rom_info_t  *pTmp = pFw_info->pRom_info;
                    while( pTmp->next )
                        pTmp = pTmp->next;

                    pTmp->next = pCur_rom_info;
                }
            }
        }
    }


    return rval;
}

static int
_output_rom_merge_list(
    fw_info_t       *pFw_info,
    const char      *pOut_path,
    out_args_t      *pArgs)
{
    int         rval = 0;
    FILE        *fout = 0;
    uint32_t    prev_addr = 0;
    uint32_t    prev_size = 0;

    if( !(fout = fopen(pOut_path, "w")) )
    {
        rval = -1;
        err_msg("open %s fail \n", pOut_path);
        return rval;
    }

    fprintf(fout, "; %s\n\n", DECLARING_MESSAGE);

    prev_addr = pArgs->rom_merge_list.flash_start_addr;

    while( pFw_info )
    {
        fw_info_t       *pCur_fw_info = pFw_info;
        rom_info_t      *pTmp_rom_info = pCur_fw_info->pRom_info;

        pFw_info = pCur_fw_info->next;

        while( pTmp_rom_info )
        {
            uint32_t    value = 0;
            rom_info_t  *pCur_rom_info = pTmp_rom_info;

            pTmp_rom_info = pCur_rom_info->next;

            value = (prev_addr + prev_size + pArgs->rom_merge_list.alignment) / pArgs->rom_merge_list.alignment;
            value = value * pArgs->rom_merge_list.alignment;

            fprintf(fout, "    AREA    |.ARM.__at_0x%X|, DATA, READONLY\n", value);
            fprintf(fout, "    INCBIN %s\%s.bin\n\n", pArgs->rom_merge_list.pBin_dir, pCur_rom_info->rom_name);

            prev_addr = value;
            prev_size = pCur_rom_info->rom_size;
        }
    }

    fprintf(fout, "%s\n", "AREA END");

    if( fout )      fclose(fout);

    return rval;
}

static int
_output_app_bld_header(
    fw_info_t       *pFw_info,
    const char      *pOut_path,
    out_args_t      *pArgs)
{
    int         rval = 0;
    FILE        *fout = 0;
    uint32_t    fw_cnt = 0;
    uint32_t    max_rom_cnt = 0;

    if( !(fout = fopen(pOut_path, "w")) )
    {
        rval = -1;
        err_msg("open %s fail \n", pOut_path);
        return rval;
    }

    fprintf(fout, "// %s\n\n", DECLARING_MESSAGE);
    fprintf(fout, "%s", "#ifndef __app_bld_header_h__\n#define __app_bld_header_h__\n\n\n");

    while( pFw_info )
    {
        fw_info_t       *pCur_fw_info = pFw_info;

        fw_cnt++;

        pFw_info = pCur_fw_info->next;

        max_rom_cnt = (max_rom_cnt > pCur_fw_info->rom_cnt) ? max_rom_cnt : pCur_fw_info->rom_cnt;

        fprintf(fout, "#define ROM_NUM_IN_PROJ%d        %d\n\n", fw_cnt, pCur_fw_info->rom_cnt);
    }

    fprintf(fout, "#define MAXIMUM_ROM_NUM          %d\n\n", max_rom_cnt);
    fprintf(fout, "#define PROJECT_NUM              %d\n\n", fw_cnt);
    fprintf(fout, "#define _IC_RAM_REGION_BASE      0x%08x\n\n", pArgs->app_bld_header.sram_mem_bass_addr);
    fprintf(fout, "#define _IC_RAM_SIZE             0x%08x\n\n", pArgs->app_bld_header.sram_mem_size);
    fprintf(fout, "#define _IC_FLASH_REGION_BASE    0x%08x\n\n", pArgs->app_bld_header.flash_mem_bass_addr);
    fprintf(fout, "\n\n#endif\n\n");

    if( fout )      fclose(fout);

    return rval;
}

static int
_output_fw_header(
    fw_info_t       *pFw_info,
    const char      *pOut_path,
    out_args_t      *pArgs)
{
#define STRING_BUF_SIZE                     (200<<10)
#define FW_HEADER_PREFIX_MEMBER_CNT         11
    int         rval = 0;
    int         i;
    FILE        *fout = 0;
    char        *pStr_buf = 0;
    char        *pStr_fw_info = 0;
    uint32_t    fw_offset[50] = {0};
    uint32_t    fw_cnt = 0;
    uint32_t    prev_addr_offset = pArgs->fw_header.alignment;
    uint32_t    prev_size = 0;
    uint32_t    offset_AES_cnt = FW_HEADER_PREFIX_MEMBER_CNT;  // 11 members in prefix (host mark ~ total F/W number)


    if( !(fout = fopen(pOut_path, "w")) )
    {
        rval = -1;
        err_msg("open %s fail \n", pOut_path);
        return rval;
    }

    if( !(pStr_buf = malloc(STRING_BUF_SIZE)) )
    {
        rval = -1;
        err_msg("malloc %d fail \n", STRING_BUF_SIZE);
        return rval;
    }
    memset(pStr_buf, 0x0, STRING_BUF_SIZE);


    PUSH_STRING(pStr_buf, 0, "; %s\n\n", DECLARING_MESSAGE);
    PUSH_STRING(pStr_buf, 1, "AREA    |.ARM.__at_0x%08X|, DATA, READONLY\n", pArgs->fw_header.flash_start_addr);
    PUSH_STRING(pStr_buf, 1, "%s", "MARK\n");


    PUSH_STRING(pStr_buf, 1, "%s", "\n; ==== host mark info (8 characters) ====\n");
    PUSH_STRING(pStr_buf, 1, "DCD 0x%08x\n    DCD 0x%08x\n",
                BIG_ENDIAN(*((uint32_t*)pArgs->fw_header.host_mark)),
                BIG_ENDIAN(*((uint32_t*)pArgs->fw_header.host_mark + 1)));

    PUSH_STRING(pStr_buf, 1, "%s", "\n; ==== uid mark info (16 byte) ====\n");
    PUSH_STRING(pStr_buf, 1, "DCD 0x%08x\n", pArgs->fw_header.uid_mark_0);
    PUSH_STRING(pStr_buf, 1, "DCD 0x%08x\n", pArgs->fw_header.uid_mark_1);

    PUSH_STRING(pStr_buf, 1, "%s", "\n; ==== md5 info (16 bytes) ====\n");
    PUSH_STRING(pStr_buf, 1, "DCD 0x%08x\n", pArgs->fw_header.md5[0]);
    PUSH_STRING(pStr_buf, 1, "DCD 0x%08x\n", pArgs->fw_header.md5[1]);
    PUSH_STRING(pStr_buf, 1, "DCD 0x%08x\n", pArgs->fw_header.md5[2]);
    PUSH_STRING(pStr_buf, 1, "DCD 0x%08x\n\n", pArgs->fw_header.md5[3]);

    PUSH_STRING(pStr_buf, 1, "%s", "\n; ==== AES info (16 bytes) ====\n");
    PUSH_STRING(pStr_buf, 1, "DCD 0x%08x ; Enable AES or not\n", pArgs->fw_header.bEnable_AES);

    if( !(pStr_fw_info = malloc(3 << 10)) )
    {
        rval = -1;
        err_msg("malloc fail \n");
        return rval;
    }
    memset(pStr_fw_info, 0x0, 3 << 10);

    while( pFw_info )
    {
        uint32_t        rom_cnt = 0;
        fw_info_t       *pCur_fw_info = pFw_info;
        rom_info_t      *pTmp_rom_info = pCur_fw_info->pRom_info;

        pFw_info = pCur_fw_info->next;

        fw_cnt++;

        PUSH_STRING(pStr_fw_info, 1, "\n; ==== fw info %d ====\n", fw_cnt);
        PUSH_STRING(pStr_fw_info, 1, "DCD 0x%08x    ; Configuration\n", 0);                                     fw_offset[fw_cnt]++;
        PUSH_STRING(pStr_fw_info, 1, "DCD 0x%08x    ; F/W mark\n", pCur_fw_info->fw_uid);                       fw_offset[fw_cnt]++;
        PUSH_STRING(pStr_fw_info, 1, "DCD 0x%08x    ; Rom Number In Project\n\n", pCur_fw_info->rom_cnt);       fw_offset[fw_cnt]++;

        offset_AES_cnt += 3;

        while( pTmp_rom_info )
        {
            uint32_t    value = 0;
            rom_info_t  *pCur_rom_info = pTmp_rom_info;

            pTmp_rom_info = pCur_rom_info->next;

            PUSH_STRING(pStr_fw_info, 1, "; ---- rom info %d, %s ----\n", rom_cnt, pCur_rom_info->rom_name);

            rom_cnt++;

            value = prev_addr_offset + prev_size;
            PUSH_STRING(pStr_fw_info, 1, "DCD 0x%08x    ; Address Offset\n", value);    fw_offset[fw_cnt]++;
            prev_addr_offset = value;

            PUSH_STRING(pStr_fw_info, 1, "DCD 0x%08lx    ; Destination Address\n", pCur_rom_info->base_addr);    fw_offset[fw_cnt]++;

            value = (pCur_rom_info->rom_size + pArgs->fw_header.alignment) / pArgs->fw_header.alignment;
            value *= pArgs->fw_header.alignment;
            PUSH_STRING(pStr_fw_info, 1, "DCD 0x%08x    ; Rom Size\n\n", value);    fw_offset[fw_cnt]++;
            prev_size = value;
        }

        offset_AES_cnt += (rom_cnt * 3);
    }

    PUSH_STRING(pStr_buf, 1, "DCD 0x%08x ; AES Info Offset\n", (offset_AES_cnt + fw_cnt) << 2);

    PUSH_STRING(pStr_buf, 1, "%s", "\n; ==== fw info (dynamic size) ====\n");
    PUSH_STRING(pStr_buf, 1, "DCD 0x%08x ; total F/W Number\n", fw_cnt);
    for(i = 0; i < fw_cnt; i++)
    {
        PUSH_STRING(pStr_buf, 1, "DCD 0x%08x ; Offset of F/W Info %d \n",
                    (FW_HEADER_PREFIX_MEMBER_CNT + fw_cnt + fw_offset[i]) << 2, i);
    }

    FLUSH_STRING(fout, pStr_buf);
    FLUSH_STRING(fout, pStr_fw_info);

    PUSH_STRING(pStr_buf, 1, "%s", "\n; AES data\n");
    PUSH_STRING(pStr_buf, 1, "%s", "END\n");
    FLUSH_STRING(fout, pStr_buf);

    if( fout )              fclose(fout);
    if( pStr_buf )          free(pStr_buf);
    if( pStr_fw_info )      free(pStr_fw_info);

    return rval;
}

static int
_output_end_padding_alignment(
    fw_info_t       *pFw_info,
    const char      *pOut_path,
    out_args_t      *pArgs)
{
    int         rval = 0;
    uint32_t    value = 0;
    FILE        *fout = 0;
    uint32_t    prev_addr_offset = pArgs->fw_header.alignment;
    uint32_t    prev_size = 0;

    if( !(fout = fopen(pOut_path, "w")) )
    {
        rval = -1;
        err_msg("open %s fail \n", pOut_path);
        return rval;
    }

    fprintf(fout, "; %s\n\n", DECLARING_MESSAGE);

    while( pFw_info )
    {
        fw_info_t       *pCur_fw_info = pFw_info;
        rom_info_t      *pTmp_rom_info = pCur_fw_info->pRom_info;

        pFw_info = pCur_fw_info->next;

        while( pTmp_rom_info )
        {

            rom_info_t  *pCur_rom_info = pTmp_rom_info;

            pTmp_rom_info = pCur_rom_info->next;

            prev_addr_offset = prev_addr_offset + prev_size;

            value = (pCur_rom_info->rom_size + pArgs->fw_header.alignment) / pArgs->fw_header.alignment;
            prev_size = value * pArgs->fw_header.alignment;
        }
    }

    value = prev_addr_offset + prev_size + pArgs->fw_header.flash_start_addr - 16;

    fprintf(fout,
        "AREA   |.ARM.__at_0x%08X|, DATA, READONLY\n"
        "MARK\n"
        "    DCD 0xEEEEEEEE\n"
        "    DCD 0xEEEEEEEE\n"
        "    DCD 0xEEEEEEEE\n"
        "    DCD 0xEEEEEEEE\n\n"
        "    END\n\n", value);

    if( fout )              fclose(fout);

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
    partial_read_t      *pHReader = 0;
    fw_info_t           *pFw_info = 0;
    char                *pIni_path = 0;

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

        map_file_cnt = iniparser_getint(pIni, "in_file:map_file_cnt", 0);
        if( !(pHReader = malloc(map_file_cnt * sizeof(partial_read_t))) )
        {
            rval = -1;
            err_msg("malloc %d fail \n", map_file_cnt * sizeof(partial_read_t));
            break;
        }

        for(i = 0; i < map_file_cnt; i++)
        {
            fw_info_t       *pCur_fw_info = 0;

            snprintf(str_buf, MAX_STR_LEN, "in_file:keil_map_file_path_%d", i);
            pPath = iniparser_getstring(pIni, str_buf, NULL);
            if( !pPath )
            {
                rval = -1;
                err_msg("no '%s' file !\n", str_buf);
                break;
            }

            if( (rval = _create_reader(pHReader + i, pPath)) )
                break;

            if( !(pCur_fw_info = malloc(sizeof(fw_info_t))) )
            {
                rval = -1;
                err_msg("malloc fw info (%d) fail !\n", sizeof(fw_info_t));
                break;
            }
            memset(pCur_fw_info, 0x0, sizeof(fw_info_t));

            snprintf(str_buf, MAX_STR_LEN, "in_file:fw_mark_%d", i);
            pCur_fw_info->fw_uid = strtoul(iniparser_getstring(pIni, str_buf, NULL), NULL, 16);

            _parse_map_file(pHReader + i, pCur_fw_info);

            // add node to fw list
            if( !pFw_info )
                pFw_info = pCur_fw_info;
            else
            {
                fw_info_t   *pTmp = pFw_info;
                while( pTmp->next )
                    pTmp = pTmp->next;

                pTmp->next = pCur_fw_info;
            }
        }

        if( rval )  break;

        #if 0 // debug message
        while( pFw_info )
        {
            fw_info_t       *pCur_fw_info = pFw_info;

            pFw_info = pCur_fw_info->next;
            printf("\n\nrom cnt = %d\n", pCur_fw_info->rom_cnt);
            while( pCur_fw_info->pRom_info )
            {
                rom_info_t  *pCur_rom_info = pCur_fw_info->pRom_info;

                pCur_fw_info->pRom_info = pCur_rom_info->next;

                printf("\tname: %s\n", pCur_rom_info->rom_name);
                printf("\tbase: x%08x\n", pCur_rom_info->base_addr);
                printf("\tsize: x%08x\n", pCur_rom_info->rom_size);
                printf("\tmax : x%08x\n\n", pCur_rom_info->rom_max_size);
            }
        }
        #endif

        {   // generate output file
            char        *pTmp = 0;
            out_args_t  out_args = {0};


            //--------------------------------
            memset(&out_args, 0x0, sizeof(out_args));
            out_args.rom_merge_list.alignment        = iniparser_getint(pIni, "flash:fw_aligmnet", 0);
            out_args.rom_merge_list.flash_start_addr = strtoul(iniparser_getstring(pIni, "flash:fw_start_addr", NULL), NULL, 16);

            pPath = (char*)iniparser_getstring(pIni, "bin:target_bin_dir", NULL);
            out_args.rom_merge_list.pBin_dir = (char*)pPath;

            while( (pTmp = strstr(pPath, "/")) )
                *pTmp = '\\';

            snprintf(str_buf, MAX_STR_LEN, "%s", "out_file:rom_merge_list_path");
            pPath = iniparser_getstring(pIni, str_buf, NULL);
            if( !pPath )
            {
                rval = -1;
                err_msg("no '%s' file !\n", str_buf);
                break;
            }
            _output_rom_merge_list(pFw_info, pPath, &out_args);

            //--------------------------------
            memset(&out_args, 0x0, sizeof(out_args));
            out_args.app_bld_header.flash_mem_bass_addr = strtoul(iniparser_getstring(pIni, "flash:flash_mem_bass_addr", NULL), NULL, 16);
            out_args.app_bld_header.sram_mem_bass_addr  = strtoul(iniparser_getstring(pIni, "ram:sram_mem_bass_addr", NULL), NULL, 16);
            out_args.app_bld_header.sram_mem_size       = strtoul(iniparser_getstring(pIni, "ram:sram_mem_size", NULL), NULL, 16);
            snprintf(str_buf, MAX_STR_LEN, "%s", "out_file:app_bld_h_path");
            pPath = iniparser_getstring(pIni, str_buf, NULL);
            if( !pPath )
            {
                rval = -1;
                err_msg("no '%s' file !\n", str_buf);
                break;
            }
            _output_app_bld_header(pFw_info, pPath, &out_args);

            //--------------------------------
            memset(&out_args, 0x0, sizeof(out_args));
            out_args.fw_header.fw_num           = map_file_cnt;
            out_args.fw_header.alignment        = iniparser_getint(pIni, "flash:fw_aligmnet", 0);
            out_args.fw_header.flash_start_addr = strtoul(iniparser_getstring(pIni, "flash:fw_start_addr", NULL), NULL, 16);
            memcpy(out_args.fw_header.host_mark,
                   iniparser_getstring(pIni, "tag:host_mark", "unknown"),
                   sizeof(out_args.fw_header.host_mark));

            out_args.fw_header.uid_mark_0 = strtoul(iniparser_getstring(pIni, "tag:uid_mark_0", NULL), NULL, 16);
            out_args.fw_header.uid_mark_1 = strtoul(iniparser_getstring(pIni, "tag:uid_mark_1", NULL), NULL, 16);

            snprintf(str_buf, MAX_STR_LEN, "%s", "out_file:fw_header_path");
            pPath = iniparser_getstring(pIni, str_buf, NULL);
            if( !pPath )
            {
                rval = -1;
                err_msg("no '%s' file !\n", str_buf);
                break;
            }
            _output_fw_header(pFw_info, pPath, &out_args);


            //--------------------------------
            memset(&out_args, 0x0, sizeof(out_args));
            out_args.fw_header.alignment = iniparser_getint(pIni, "flash:fw_aligmnet", 0);
            out_args.fw_header.flash_start_addr = strtoul(iniparser_getstring(pIni, "flash:fw_start_addr", NULL), NULL, 16);
            snprintf(str_buf, MAX_STR_LEN, "%s", "out_file:fw_end_padding_s_path");
            pPath = iniparser_getstring(pIni, str_buf, NULL);
            if( !pPath )
            {
                rval = -1;
                err_msg("no '%s' file !\n", str_buf);
                break;
            }
            _output_end_padding_alignment(pFw_info, pPath, &out_args);
        }
    } while(0);

    if( pHReader )
    {
        for(i = 0; i < map_file_cnt; i++)
            _destroy_reader(pHReader + i);

        free(pHReader);
    }

    while( pFw_info )
    {
        fw_info_t   *pCur = pFw_info;

        pFw_info = pCur->next;

        // free rom info
        while( pCur->pRom_info )
        {
            rom_info_t  *pCur_rom_info = pCur->pRom_info;

            pCur->pRom_info = pCur_rom_info->next;
            free(pCur_rom_info);
        }

        free(pCur);
    }

    return 0;
}

