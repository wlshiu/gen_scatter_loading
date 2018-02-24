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

#define MAX_LOAD_REGION_NUM         10
#define MAX_EXEC_REGION_NUM         10

//=============================================================================
//                  Macro Definition
//=============================================================================
#define err_msg(str, args...)           do{ fprintf(stderr, "%s[%u] " str, __func__, __LINE__, ##args); while(1);}while(0)

#define dbg_msg(str, args...)           fprintf(stderr, "%s[%u] " str, __func__, __LINE__, ##args);

#define REGEX_MATCH_EXTRACT(pBuf, pLine_str, regmatch_info, match_idx)     \
    strncpy(pBuf, &pLine_str[regmatch_info[match_idx].rm_so], regmatch_info[match_idx].rm_eo - regmatch_info[match_idx].rm_so)

#define BIG_ENDIAN(x)       ((((x) & 0xff) << 24) | (((x) & 0xff00) << 8) | (((x) & 0xff0000) >> 8) | (((x) & 0xff000000) >> 24))

#define PUSH_TO_LIST(struct_type, ppRoot, pNew)                                 \
            do{ if( *(ppRoot) ) {                                               \
                    struct_type     *pTmp = (struct_type *)(*(ppRoot));         \
                    while( pTmp->next )  pTmp = pTmp->next;                     \
                    pTmp->next = pNew;                                          \
                } else {                                                        \
                    *(ppRoot) = pNew;                                           \
                }                                                               \
            }while(0)
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

    bool        is_used;

} obj_info_t;

typedef struct obj_info_db
{
    uint32_t        total_obj_num;
    obj_info_t      *pObj_info;

} obj_info_db_t;

/**
 *  region info
 */
typedef struct region_info
{
    char        region_addr[16];
    uint32_t    region_size;

} region_info_t;

typedef struct rom_region
{
    struct rom_region   *next;

    region_info_t   load_region;

    uint32_t        exec_region_num;
    region_info_t   exec_region[MAX_EXEC_REGION_NUM];

} rom_region_t;

/**
 *  mgr
 */
typedef struct rom_region_mgr
{
    uint32_t        rom_num;
    char            exec_region_prefix[64];
    rom_region_t    *pRom_region;

} rom_region_mgr_t;

/**
 *  default Objects
 */
typedef struct def_obj
{
    struct def_obj      *next;
    char                obj_name[64];

} def_obj_t;

/**
 *  default objects in a region
 */
typedef struct region_def_obj
{
    uint32_t    def_obj_num;
    def_obj_t   *pDef_obj;

} region_def_obj_t;

/**
 *  mgr
 */
typedef struct def_obj_mgr
{
    region_def_obj_t   load_region_obj[MAX_LOAD_REGION_NUM];

} def_obj_mgr_t;
//=============================================================================
//                  Global Data Definition
//=============================================================================
static obj_info_db_t        g_obj_info_db = {0};
static def_obj_mgr_t        g_def_obj_mgr = {0};
static rom_region_mgr_t     g_rom_region_mgr = {0};
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
    obj_info_db_t   *pObj_info_db)
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

            { // check match items
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
                    snprintf(pObj_info_cur->obj_name, 64, "%s.o", str_buf);
                }

                PUSH_TO_LIST(obj_info_t, &pObj_info_db->pObj_info, pObj_info_cur);
                pObj_info_db->total_obj_num++;
            }
        }
    }

    regfree(&hRegex_obj_info);

    return rval;
}


static int
_parse_def_obj(
    dictionary      *pIni,
    def_obj_mgr_t   *pDef_obj_mgr)
{
    int         rval = 0;

    do{
        int         i, obj_cnt = 0;
        char        str_buf[MAX_STR_LEN] = {0};

        for(i = 0; i < MAX_LOAD_REGION_NUM; i++)
        {
            int                 num = 0;
            region_def_obj_t    *pRegion_obj_cur = &pDef_obj_mgr->load_region_obj[i];

            snprintf(str_buf, MAX_STR_LEN, "obj:load_region_%d_obj_num", i + 1);
            pRegion_obj_cur->def_obj_num = num = iniparser_getint(pIni, str_buf, 0);
            if( pRegion_obj_cur->def_obj_num == 0 )
                continue;

            for(obj_cnt = 0; obj_cnt < pRegion_obj_cur->def_obj_num; obj_cnt++)
            {
                const char      *pName = 0;
                def_obj_t       *pDef_obj_new = 0;

                snprintf(str_buf, MAX_STR_LEN, "obj:load_region_%d_obj_%d", i + 1, obj_cnt + 1);
                pName = iniparser_getstring(pIni, str_buf, NULL);
                if( !pName )
                {
                    rval = -1;
                    err_msg("no '%s' item !\n", str_buf);
                    break;
                }

                if( !(pDef_obj_new = malloc(sizeof(def_obj_t))) )
                {
                    rval = -1;
                    err_msg("malloc %d fail !\n", sizeof(def_obj_t));
                    break;
                }
                memset(pDef_obj_new, 0x0, sizeof(def_obj_t));

                snprintf(pDef_obj_new->obj_name, 64, "%s", pName);

                PUSH_TO_LIST(def_obj_t, &pRegion_obj_cur->pDef_obj, pDef_obj_new);
            }

            if( rval )      break;
        }
    }while(0);

    return rval;
}

static int
_parse_rom_region(
    dictionary          *pIni,
    rom_region_mgr_t    *pRom_region_mgr)
{
    int     rval = 0;

    do{
        int         i;
        int         num = 0;
        char        str_buf[MAX_STR_LEN] = {0};
        const char  *pPrefix = 0;

        num = iniparser_getint(pIni, "rom:rom_num", 0);
        pRom_region_mgr->rom_num = num;

        pPrefix = iniparser_getstring(pIni, "rom:rom_exec_region_prefix", NULL);
        if( !pPrefix )
        {
            rval = -1;
            err_msg("no '%s' item !\n", str_buf);
            break;
        }

        snprintf(pRom_region_mgr->exec_region_prefix, 64, "%s", pPrefix);

        //-------------------------------------------
        // get load region info
        for(i = 0; i < pRom_region_mgr->rom_num; i++)
        {
            int             exec_region_cnt = 0;
            rom_region_t    *pRom_region_new = 0;
            const char      *pLoad_region_addr = 0;
            const char      *pRegion_size = 0;

            snprintf(str_buf, MAX_STR_LEN, "rom:rom_load_region_addr_%d", i + 1);
            pLoad_region_addr = iniparser_getstring(pIni, str_buf, NULL);
            if( !pLoad_region_addr )
            {
                rval = -1;
                err_msg("no '%s' item !\n", str_buf);
                break;
            }

            snprintf(str_buf, MAX_STR_LEN, "rom:rom_load_region_size_%d", i + 1);
            pRegion_size = iniparser_getstring(pIni, str_buf, NULL);
            if( !pRegion_size )
            {
                rval = -1;
                err_msg("no '%s' item !\n", str_buf);
                break;
            }

            if( !(pRom_region_new = malloc(sizeof(rom_region_t))) )
            {
                rval = -1;
                err_msg("malloc %s fail !\n", sizeof(rom_region_t));
                break;
            }
            memset(pRom_region_new, 0x0, sizeof(rom_region_t));

            snprintf(pRom_region_new->load_region.region_addr, 16, "%s", pLoad_region_addr);
            pRom_region_new->load_region.region_size = (pRegion_size[0] == '0' && pRegion_size[1] == 'x')
                                                     ? strtoul(&pRegion_size[2], NULL, 16)
                                                     : strtoul(pRegion_size, NULL, 10);

            //-------------------------------------------
            // get exec region info in a load region
            snprintf(str_buf, MAX_STR_LEN, "rom:rom_load_region_%d_exec_num", i + 1);
            pRom_region_new->exec_region_num = num = iniparser_getint(pIni, str_buf, 0);

            for(exec_region_cnt = 0; exec_region_cnt < pRom_region_new->exec_region_num; exec_region_cnt++)
            {
                const char          *pExec_region_addr = 0;
                region_info_t       *pRegion_info_cur = &pRom_region_new->exec_region[exec_region_cnt];

                snprintf(str_buf, MAX_STR_LEN, "rom:rom_exec_region_addr_%d_%d", i + 1, exec_region_cnt + 1);
                pExec_region_addr = iniparser_getstring(pIni, str_buf, NULL);
                if( !pExec_region_addr )
                {
                    rval = -1;
                    err_msg("no '%s' item !\n", str_buf);
                    break;
                }

                snprintf(str_buf, MAX_STR_LEN, "rom:rom_exec_region_size_%d_%d", i + 1, exec_region_cnt + 1);
                pRegion_size = iniparser_getstring(pIni, str_buf, NULL);
                if( !pRegion_size )
                {
                    rval = -1;
                    err_msg("no '%s' item !\n", str_buf);
                    break;
                }

                snprintf(pRegion_info_cur->region_addr, 16, "%s", pExec_region_addr);
                pRegion_info_cur->region_size = (pRegion_size[0] == '0' && pRegion_size[1] == 'x')
                                              ? strtoul(&pRegion_size[2], NULL, 16)
                                              : strtoul(pRegion_size, NULL, 10);
            }

            PUSH_TO_LIST(rom_region_t, &pRom_region_mgr->pRom_region, pRom_region_new);
        }
    }while(0);
    return rval;
}

static int
_gen_sct_file(
    const char          *pOut_path,
    obj_info_db_t       *pObj_info_db,
    def_obj_mgr_t       *pDef_obj_mgr,
    rom_region_mgr_t    *pRom_region_mgr)
{
    #define STRING_BUF_SIZE (200<<10)
    int     rval = 0;

    FILE    *fout = 0;
    char    *pStr_buf = 0;

    do{
        int     i, j;
        if( !(fout = fopen(pOut_path, "wb")) )
        {
            rval = -1;
            err_msg("open '%s' fail !\n", pOut_path);
            break;
        }

        if( !(pStr_buf = malloc(STRING_BUF_SIZE)) )
        {
            rval = -1;
            err_msg("malloc %d fail !\n", STRING_BUF_SIZE);
            break;
        }
        memset(pStr_buf, 0x0, STRING_BUF_SIZE);

        PUSH_STRING(pStr_buf, 0, "%s", "#! armcc -E -I ..\\src\n\n");
        PUSH_STRING(pStr_buf, 0, "%s", "; *************************************************************\n");
        PUSH_STRING(pStr_buf, 0, "%s", "; *** Scatter-Loading Description File generated by uVision ***\n");
        PUSH_STRING(pStr_buf, 0, "%s", "; *************************************************************\n\n");
        FLUSH_STRING(fout, pStr_buf);

        for(i = 0; i < pRom_region_mgr->rom_num; i++)
        {
            int                 cnt = 0;
            rom_region_t        *pRom_region_cur = pRom_region_mgr->pRom_region;
            rom_region_t        *pRom_region_act = 0;
            region_def_obj_t    *pRegion_def_obj_cur = &pDef_obj_mgr->load_region_obj[i];

            //--------------------------------------
            // get target region info
            while( pRom_region_cur )
            {
                if( cnt == i )
                {
                    pRom_region_act = pRom_region_cur;
                    break;
                }

                pRom_region_cur = pRom_region_cur->next;
                cnt++;
            }

            if( !pRom_region_act )
            {
                err_msg("%s", "no ROM region info !\n");
                break;
            }

            //-------------------------------------------
            // output load region info
            PUSH_STRING(pStr_buf, 0, "Load_Region_IROM%d %s 0x%08X {\n\n",
                        i + 1,
                        pRom_region_act->load_region.region_addr,
                        pRom_region_act->load_region.region_size);

            //----------------------------------------
            // multi exec regions in a load region
            for(j = 0; j < pRom_region_act->exec_region_num; j++)
            {
                uint32_t            remain_size = 0;
                bool                is_next_region = false;
                def_obj_t           *pDef_obj_cur = pRegion_def_obj_cur->pDef_obj;
                region_info_t       *pExec_region_cur = &pRom_region_act->exec_region[j];

                PUSH_STRING(pStr_buf, 1, "Exec_Region_%s_IROM%d_%d %s 0x%08X {\n",
                            pRom_region_mgr->exec_region_prefix,
                            i + 1,
                            j + 1,
                            pExec_region_cur->region_addr,
                            pExec_region_cur->region_size);

                remain_size = pExec_region_cur->region_size;

                //----------------------------------------
                // put default objects to exec region
                while( pDef_obj_cur )
                {
                    def_obj_t       *pDef_obj_act = pDef_obj_cur;
                    obj_info_t      *pObj = pObj_info_db->pObj_info;
                    uint32_t        len = 0;
                    bool            is_valid = false;

                    pDef_obj_cur = pDef_obj_cur->next;

                    //-----------------------------------
                    // verify the default object in object's database
                    len = strlen(pDef_obj_act->obj_name);
                    while( pObj )
                    {
                        if( strncmp(pDef_obj_act->obj_name, pObj->obj_name, len) == 0 )
                        {
                            is_valid = true;
                            break;
                        }
                        pObj = pObj->next;
                    }

                    //------------------------------
                    // output default objects
                    if( is_valid )
                    {
                        if( pObj->is_used == true )
                            continue;

                        if( remain_size < (pObj->code_size + pObj->rw_data_size + pObj->zi_data_size) )
                        {
                            printf("default object '%s' is not enough space in 'Exec_Region_%s_IROM%d_%d'\n",
                                   pObj->obj_name, pRom_region_mgr->exec_region_prefix,
                                   i + 1, j + 1);
                            is_next_region = true;
                            break;
                        }

                        PUSH_STRING(pStr_buf, 2, "%s (+RO) ; size=%d\n", pObj->obj_name, pObj->code_size);
                        remain_size -= pObj->code_size;

                        if( pObj->rw_data_size )
                        {
                            PUSH_STRING(pStr_buf, 2, "%s (+RW) ; size=%d\n", pObj->obj_name, pObj->rw_data_size);
                            remain_size -= pObj->rw_data_size;
                        }

                        if( pObj->zi_data_size )
                        {
                            PUSH_STRING(pStr_buf, 2, "%s (+ZI) ; size=%d\n", pObj->obj_name, pObj->zi_data_size);
                            remain_size -= pObj->zi_data_size;
                        }

                        pObj->is_used = true;
                        pObj_info_db->total_obj_num--;
                    }
                    else
                    {
                        printf("the default object '%s' is not exist !!\n", pDef_obj_act->obj_name);
                        is_next_region = true;
                    }
                }

                if( is_next_region == true )
                {
                    PUSH_STRING(pStr_buf, 1, "} ; remain %d\n\n", remain_size);
                    FLUSH_STRING(fout, pStr_buf);
                    continue;
                }

                //----------------------------------------------
                // put objects which are not the default objects to this exec region
                // ps. it also should check the other default objects in the other exec region
                while( remain_size && pObj_info_db->total_obj_num )
                {
                    obj_info_t      *pObj_cur = pObj_info_db->pObj_info;

                    while( pObj_cur )
                    {
                        int             k;
                        bool            is_valid = true;
                        obj_info_t      *pObj = pObj_cur;

                        pObj_cur = pObj_cur->next;

                        // object has be used
                        if( pObj->is_used == true )
                            continue;

                        // check current object is not default objects
                        for(k = 0; k < MAX_LOAD_REGION_NUM; k++)
                        {
                            region_def_obj_t    *pRegion_def_obj_cur = &pDef_obj_mgr->load_region_obj[k];

                            pDef_obj_cur = pRegion_def_obj_cur->pDef_obj;
                            while( pDef_obj_cur )
                            {
                                def_obj_t       *pDef_obj_act = pDef_obj_cur;
                                uint32_t        len = 0;

                                pDef_obj_cur = pDef_obj_cur->next;
                                len = strlen(pDef_obj_act->obj_name);
                                if( strncmp(pObj->obj_name, pDef_obj_act->obj_name, len) == 0 )
                                {
                                    is_valid = false;
                                    break;
                                }
                            }

                            if( is_valid == false )
                                break;
                        }

                        if( is_valid == false )
                            continue;

                        // check space is enough or not
                        if( remain_size < (pObj->code_size + pObj->rw_data_size + pObj->zi_data_size) )
                        {
                            is_next_region = true;
                            break;
                        }

                        //----------------------
                        // output object
                        PUSH_STRING(pStr_buf, 2, "%s (+RO) ; size=%d\n", pObj->obj_name, pObj->code_size);
                        remain_size -= pObj->code_size;

                        if( pObj->rw_data_size )
                        {
                            PUSH_STRING(pStr_buf, 2, "%s (+RW) ; size=%d\n", pObj->obj_name, pObj->rw_data_size);
                            remain_size -= pObj->rw_data_size;
                        }

                        if( pObj->zi_data_size )
                        {
                            PUSH_STRING(pStr_buf, 2, "%s (+ZI) ; size=%d\n", pObj->obj_name, pObj->zi_data_size);
                            remain_size -= pObj->zi_data_size;
                        }

                        pObj->is_used = true;
                        pObj_info_db->total_obj_num--;
                    }

                    if( is_next_region == true )
                        break;
                }

                PUSH_STRING(pStr_buf, 1, "} ; remain %d\n\n", remain_size);
                FLUSH_STRING(fout, pStr_buf);
            }

            PUSH_STRING(pStr_buf, 0, "%s", "}\n\n");
            FLUSH_STRING(fout, pStr_buf);
        }

        // TODO: check unused objects in database

    }while(0);

    if( pStr_buf )  free(pStr_buf);
    if( fout )      fclose(fout);
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

        snprintf(str_buf, MAX_STR_LEN, "%s", "file:map_file");
        pPath = iniparser_getstring(pIni, str_buf, NULL);
        if( !pPath )
        {
            rval = -1;
            err_msg("no '%s' file !\n", str_buf);
            break;
        }

        if( (rval = _create_reader(pHReader , pPath)) )
            break;

        _parse_map_file(pHReader, &g_obj_info_db);

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

                fprintf(fout, "\t%d\t%d\t%d\t%d\t%d\t%d\t%s\n",
                       pcur->code_size, pcur->inc_data_size, pcur->ro_data_size, pcur->rw_data_size,
                       pcur->zi_data_size, pcur->debug_size, pcur->obj_name);
            }

            if( fout )      fclose(fout);
        }
        #endif

        _parse_def_obj(pIni, &g_def_obj_mgr);

        _parse_rom_region(pIni, &g_rom_region_mgr);

        snprintf(str_buf, MAX_STR_LEN, "%s", "file:out_sct_file");
        pPath = iniparser_getstring(pIni, str_buf, NULL);
        if( !pPath )
        {
            rval = -1;
            err_msg("no '%s' file !\n", str_buf);
            break;
        }

        _gen_sct_file(pPath, &g_obj_info_db, &g_def_obj_mgr, &g_rom_region_mgr);

    } while(0);

    if( g_obj_info_db.pObj_info )
    {
        obj_info_t       *pObj_cur = g_obj_info_db.pObj_info;
        while( pObj_cur )
        {
            obj_info_t       *pObj_tmp = pObj_cur;

            pObj_cur = pObj_cur->next;
            free(pObj_tmp);
        }
    }

    for(i = 0; i < MAX_LOAD_REGION_NUM; i++)
    {
        region_def_obj_t    *pRegion_obj_cur = &g_def_obj_mgr.load_region_obj[i];
        def_obj_t           *pObj_cur = 0;

        if( pRegion_obj_cur->def_obj_num == 0 )
            continue;

        pObj_cur = pRegion_obj_cur->pDef_obj;
        while( pObj_cur )
        {
            def_obj_t       *pObj_tmp = pObj_cur;
            pObj_cur = pObj_cur->next;
            free(pObj_tmp);
        }
    }

    if( g_rom_region_mgr.pRom_region )
    {
        rom_region_t       *pRom_cur = g_rom_region_mgr.pRom_region;
        while( pRom_cur )
        {
            rom_region_t       *pRom_tmp = pRom_cur;

            pRom_cur = pRom_cur->next;
            free(pRom_tmp);
        }
    }

    _destroy_reader(pHReader);

    return 0;
}

