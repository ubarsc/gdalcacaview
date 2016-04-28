/*
 *  gdalcacaview  GDAL image viewer for libcaca
 *  Copyright (c) 2016 Sam Gillingham <gillingham.sam@gmail.com>
 *                All Rights Reserved
 *
 *  This program is free software. It comes without any warranty, to
 *  the extent permitted by applicable law. You can redistribute it
 *  and/or modify it under the terms of the Do What The Fuck You Want
 *  To Public License, Version 2, as published by Sam Hocevar. See
 *  http://sam.zoy.org/wtfpl/COPYING for more details.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#if !defined(_WIN32) || defined(__CYGWIN__) 
#include <sys/types.h>
#include <pwd.h>
#endif

#include "caca.h"
#include "gdal.h"
#include "cpl_string.h"

/* Local macros */
#define STATUS_DITHERING 1
#define STATUS_ANTIALIASING 2
#define STATUS_BACKGROUND 3

#define ZOOM_IN_FACTOR 0.9
#define ZOOM_OUT_FACTOR 1.1
#define PAN_STEP 0.2
#define GAMMA_FACTOR 1.04f
#define GAMMA_MAX 100
#define GAMMA(g) (((g) < 0) ? 1.0 / gammatab[-(g)] : gammatab[(g)])
#define PIX_PER_CELL 30
#define GEOLINK_TIMEOUT 1000000

/* tell libcaca how we have encoded the bytes */
/* red, then green, then blue */    
#define RMASK 0x0000ff
#define GMASK 0x00ff00
#define BMASK 0xff0000
#define AMASK 0x000000
#define IMG_DEPTH 3

/* libcaca/libcaca contexts */
caca_canvas_t *cv; caca_display_t *dp;

/* Area for printing GDAL error messages */
#define GDAL_ERROR_SIZE 1024
char szGDALMessages[GDAL_ERROR_SIZE];

/* Default stretch rules */
char *pszDefaultStretchRules[] = {"equal,1,1:colortable,none,,1",
"equal,1,-1:greyscale,none,,1",
"equal,2,-1:greyscale,none,,1",
"equal,3,-1:rgb,none,,1|2|3",
"less,6,-1:rgb,stddev,2.0,4|3|2",
"greater,5,-1:rgb,stddev,2.0,5|4|2", NULL};


/* stolen from common-image.h */
struct image
{
    char *pixels;
    unsigned int w, h;
    struct caca_dither *dither;
    void *priv;
};

/* constants from TuiView */
#define VIEWER_COMP_LT 0
#define VIEWER_COMP_GT 1
#define VIEWER_COMP_EQ 2

/* constants for specifying how to display an image */
#define VIEWER_MODE_COLORTABLE 1
#define VIEWER_MODE_GREYSCALE 2
#define VIEWER_MODE_RGB 3
#define VIEWER_MODE_PSEUDOCOLOR 4 /* not currently supported */

/* how to stretch an image */
#define VIEWER_STRETCHMODE_NONE 1 /* color table, or pre stretched data */
#define VIEWER_STRETCHMODE_LINEAR 2
#define VIEWER_STRETCHMODE_STDDEV 3
#define VIEWER_STRETCHMODE_HIST 4

struct stretch
{
    /* rule part */
    int comp; /* one of VIEWER_COMP_* values */
    int value;
    int ctband; /* or -1 */

    /* actual stretch part */
    int mode; /* one of VIEWER_MODE_* */
    int stretchmode;     /* VIEWER_STRETCHMODE_* */
    double stretchparam[2];
    int bands[3];
};

struct stretchlist
{
    int num_stretches;
    struct stretch* stretches;
};

struct extent
{
    double dCentreX, dCentreY, dMetersPerCell;
};

struct gdalFile
{
    GDALDatasetH ds;
    struct extent fullExtent;
    struct stretch *stretch;
    double adfTransform[6];
};

/* Local functions */
static void print_status(void);
static void print_help(int, int);
static void set_gamma(int);
static void draw_checkers(int, int, int, int);

int gdal_open_file(char const *, struct gdalFile*, struct stretchlist *, struct stretch*);
void gdal_close_file(struct gdalFile*);
extern struct image * gdal_load_image(struct gdalFile*, struct extent *);
extern void gdal_unload_image(struct image *);

/* Local variables */
struct image *im = NULL;

float gammatab[GAMMA_MAX + 1];
int g = 0, mode, ww, wh;
char *pszStretchStatusString = NULL;

/* Takes the rule part (left of :) and puts it into the stretch */
int rulepart_from_string(struct stretch *newStretch, const char *pszString)
{
char **pszTokens;
char *pszTmp;
int n = 0;

    pszTokens = CSLTokenizeString2(pszString, ",", 
        CSLT_STRIPLEADSPACES | CSLT_STRIPENDSPACES | CSLT_ALLOWEMPTYTOKENS);

    pszTmp = pszTokens[n];
    if( pszTmp == NULL )
    {
        fprintf(stderr, "Missing comparison in rule string\n");
        CSLDestroy(pszTokens);
        return 0;
    }
    if( strcmp(pszTmp, "less") == 0 )
        newStretch->comp = VIEWER_COMP_LT;
    else if( strcmp(pszTmp, "greater") == 0)
        newStretch->comp = VIEWER_COMP_GT;
    else if( strcmp(pszTmp, "equal") == 0)
        newStretch->comp = VIEWER_COMP_EQ;
    else
    {
        fprintf(stderr, "Unable to understand comparison %s\n", pszTmp);
        CSLDestroy(pszTokens);
        return 0;
    }

    n++;
    pszTmp = pszTokens[n];
    if( pszTmp == NULL )
    {
        fprintf(stderr, "Missing comparison value in rule string\n");
        CSLDestroy(pszTokens);
        return 0;
    }
    newStretch->value = atol(pszTmp);    

    n++;
    pszTmp = pszTokens[n];
    if( pszTmp == NULL )
    {
        fprintf(stderr, "Missing color table band value in rule string\n");
        CSLDestroy(pszTokens);
        return 0;
    }
    newStretch->ctband = atol(pszTmp);

    CSLDestroy(pszTokens);

    return 1;
}

/* deals with the part to the right of the : */
int stretchpart_from_string(struct stretch *newStretch, const char *pszString)
{
char **pszTokens;
char **pszExtraTokens;
char *pszTmp;
int n = 0, i;

    pszTokens = CSLTokenizeString2(pszString, ",", 
        CSLT_STRIPLEADSPACES | CSLT_STRIPENDSPACES | CSLT_ALLOWEMPTYTOKENS);

    pszTmp = pszTokens[n];
    if( pszTmp == NULL )
    {
        fprintf(stderr, "Missing value in rule string\n");
        CSLDestroy(pszTokens);
        return 0;
    }
    if( strcmp(pszTmp, "colortable") == 0 )
        newStretch->mode = VIEWER_MODE_COLORTABLE;
    else if( strcmp(pszTmp, "greyscale") == 0)
        newStretch->mode = VIEWER_MODE_GREYSCALE;
    else if( strcmp(pszTmp, "rgb") == 0)
        newStretch->mode = VIEWER_MODE_RGB;
    else
    {
        fprintf(stderr, "Unable to understand stretch mode %s\n", pszTmp);
        CSLDestroy(pszTokens);
        return 0;
    }

    n++;
    pszTmp = pszTokens[n];
    if( pszTmp == NULL )
    {
        fprintf(stderr, "Missing value in rule string\n");
        CSLDestroy(pszTokens);
        return 0;
    }
    if( (strcmp(pszTmp, "none") == 0 ) || (strcmp(pszTmp, "") == 0))
        newStretch->stretchmode = VIEWER_STRETCHMODE_NONE;
    else if( strcmp(pszTmp, "linear") == 0)
        newStretch->stretchmode = VIEWER_STRETCHMODE_LINEAR;
    else if( strcmp(pszTmp, "stddev") == 0)
        newStretch->stretchmode = VIEWER_STRETCHMODE_STDDEV;
    else if( strcmp(pszTmp, "histogram") == 0)
        newStretch->stretchmode = VIEWER_STRETCHMODE_HIST;
    else
    {
        fprintf(stderr, "Unable to understand stretch mode %s\n", pszTmp);
        CSLDestroy(pszTokens);
        return 0;
    }

    n++;
    pszTmp = pszTokens[n];
    if( pszTmp == NULL )
    {
        fprintf(stderr, "Missing value in rule string\n");
        CSLDestroy(pszTokens);
        return 0;
    }
    pszExtraTokens = CSLTokenizeString2(pszTmp, "|", CSLT_STRIPLEADSPACES | CSLT_STRIPENDSPACES);
    i = 0; 
    while( i < 2 )
    {
        pszTmp = pszExtraTokens[i];
        if( pszTmp == NULL )
            break;
        newStretch->stretchparam[i] = atof(pszTmp);
        i++;
    }
    CSLDestroy(pszExtraTokens);

    n++;
    pszTmp = pszTokens[n];
    if( pszTmp == NULL )
    {
        fprintf(stderr, "Missing value in rule string\n");
        CSLDestroy(pszTokens);
        return 0;
    }
    pszExtraTokens = CSLTokenizeString2(pszTmp, "|", CSLT_STRIPLEADSPACES | CSLT_STRIPENDSPACES);
    i = 0; 
    while( i < 3 )
    {
        pszTmp = pszExtraTokens[i];
        if( pszTmp == NULL )
            break;
        newStretch->bands[i] = atol(pszTmp);
        i++;
    }
    CSLDestroy(pszExtraTokens);

    CSLDestroy(pszTokens);

    return 1;
}

int stretch_from_string(struct stretch *newStretch, const char *pszString)
{
char **pszRuleAndStretch;
char *pszTmp;

    /* First split into the 2 parts - rule and stretch */
    pszRuleAndStretch = CSLTokenizeString2(pszString, ":",
        CSLT_STRIPLEADSPACES | CSLT_STRIPENDSPACES | CSLT_ALLOWEMPTYTOKENS);

    pszTmp = pszRuleAndStretch[0];
    if( pszTmp == NULL )
    {
        fprintf(stderr, "Missing rule string\n");
        CSLDestroy(pszRuleAndStretch);
        return 0;
    }

    if( !rulepart_from_string(newStretch, pszTmp) )
    {
        CSLDestroy(pszRuleAndStretch);
        return 0;
    }

    pszTmp = pszRuleAndStretch[1];
    if( pszTmp == NULL )
    {
        fprintf(stderr, "Missing stretch string\n");
        CSLDestroy(pszRuleAndStretch);
        return 0;
    }

    if( !stretchpart_from_string(newStretch, pszTmp) )
    {
        CSLDestroy(pszRuleAndStretch);
        return 0;
    }

    CSLDestroy(pszRuleAndStretch);

    return 1;
}

/* Returns stretch for given dataset worked out using the rules */
/* returns NULL on failure */
struct stretch *get_stretch_for_gdal(struct stretchlist *stretchList, GDALDatasetH ds)
{
int match, i, hasRed, hasGreen, hasBlue, hasAlpha, ncols, c;
int rasterCount = GDALGetRasterCount(ds);
struct stretch *pStretch;
GDALRasterBandH bandh;
GDALRasterAttributeTableH rath;
const char *psz;
GDALRATFieldUsage usage;

    for( i = 0; i < stretchList->num_stretches; i++ )
    {
        match = 0;
        pStretch = &stretchList->stretches[i];
        if( pStretch->comp == VIEWER_COMP_LT)
            match = rasterCount < pStretch->value;
        else if( pStretch->comp == VIEWER_COMP_GT)
            match = rasterCount > pStretch->value;
        else if( pStretch->comp == VIEWER_COMP_EQ)
            match = rasterCount == pStretch->value;
        else
        {
            fprintf(stderr, "invalid value for comparison\n");
            return NULL;
        }

        if( match && ( pStretch->ctband != -1) && (pStretch->ctband <= rasterCount) )
        {
            bandh = GDALGetRasterBand(ds, pStretch->ctband);
            psz = GDALGetMetadataItem(bandh, "LAYER_TYPE", NULL);
            if( (psz != NULL) && (strcmp(psz, "thematic") == 0))
            {
                hasRed = 0;
                hasGreen = 0;
                hasBlue = 0;
                hasAlpha = 0;
                rath = GDALGetDefaultRAT(bandh);
                if( rath != NULL )
                {
                    ncols = GDALRATGetColumnCount(rath);
                    for( c = 0; c < ncols; c++ )
                    {
                        usage = GDALRATGetUsageOfCol(rath, c);
                        if( usage == GFU_Red )
                            hasRed = 1;
                        if( usage == GFU_Green )
                            hasGreen = 1;
                        if( usage == GFU_Blue )
                            hasBlue = 1;
                        if( usage == GFU_Alpha )
                            hasAlpha = 1;
                    }
                }
                match = hasRed && hasGreen && hasBlue && hasAlpha;
            }
        }

        if( match )
            return pStretch;
    }

    return NULL;
}

void printUsage()
{
    printf("gdalcacaview [options] filename\n\n");

    printf("where options is one of:\n"); 
    printf(" --printdrivers\tPrint list of available drivers and exit\n");
    printf(" --driver DRIVER\tUse the specified driver. If not given, uses default\n");
    printf(" --stretch STRETCH\tUse the specified stretch string. If not given uses default stretch rules\n");
    printf(" --geolink FILE\tUse the specified file to communicate with other instances and geolink\n");
    printf("and filename is a GDAL supported dataset.\n");
}

void printDrivers()
{
    int i;
    char const* const* list;
    
    printf("Driver\tDescription\n");
    printf("------\t-----------\n");
    list = caca_get_display_driver_list();
    for( i = 0; list[i] != NULL; i+=2 )
    {
        printf( "%s\t%s\n", list[i], list[i+1] );
    }
}

int main(int argc, char **argv)
{
    char const * const * algos = caca_get_dither_algorithm_list(NULL);
    int dither_algorithm = 0;

    int quit = 0, update = 1, help = 0, status = 0;
    int reload = 0, rezoom = 0;

    int i;
    char *pszDriver = NULL;
    char *pszConfigFile = NULL, *pszHomeDir = NULL;
    char **pszConfigLines, **pszConfigSingleLine;
    char *pszFileName = NULL;
    struct stretchlist stretchList;
    struct stretch *pCmdStretch = NULL; /* non-NULL when stretch is passed in on command line */
    char *pszGeolinkFile = NULL;
    struct extent dispExtent;
    struct gdalFile gdalfile;

    memset(&dispExtent, 0, sizeof(dispExtent));
    memset(&gdalfile, 0, sizeof(gdalfile));

/* -------------------------------------------------------------------- */
/*      Read config file if it exists                                   */
/* -------------------------------------------------------------------- */
#if defined(_WIN32) && !defined(__CYGWIN__) 
    pszHomeDir = getenv("USERPROFILE");
    if( pszHomeDir == NULL )
    {
        /* '.gcv' plus seperating slash plus terminating null */
        pszConfigFile = CPLMalloc(strlen(pszHomeDir) + 6);
        /* copy in the path */
        strcpy(pszConfigFile, pszHomeDir);
        
    }
    else
    {
        i = strlen(getenv("HOMEDRIVE")) + 1 + strlen(getenv("HOMEPATH")) + 6;
        pszConfigFile = CPLMalloc(i);
        strcpy(pszConfigFile, getenv("HOMEDRIVE"));
        strcat(pszConfigFile, "\\");
        strcat(pszConfigFile, getenv("HOMEPATH"));
    }

    /* slash and filename */
    strcat(pszConfigFile, "\\.gcv");

#else
    struct passwd* pwd = getpwuid(getuid());
    if( pwd != NULL )
    {
        pszHomeDir = pwd->pw_dir;
    }
    else
    {
        pszHomeDir = getenv("HOME");
    }

    /* '.gcv' plus seperating slash plus terminating null */
    pszConfigFile = CPLMalloc(strlen(pszHomeDir) + 6);
    /* copy in the path */
    strcpy(pszConfigFile, pszHomeDir);
    /* slash and filename */
    strcat(pszConfigFile, "/.gcv");

#endif

    stretchList.num_stretches = 0;
    stretchList.stretches = NULL;

    /* don't show error if file doesn't exist */
    CPLSetErrorHandler(CPLQuietErrorHandler);
    pszConfigLines = CSLLoad(pszConfigFile);
    CPLSetErrorHandler(NULL);

    if( pszConfigLines != NULL )
    {
        /*fprintf(stderr, "reading config file %s\n", pszConfigFile);*/
        i = 0;
        while(pszConfigLines[i] != NULL)
        {
            pszConfigSingleLine = CSLTokenizeString2(pszConfigLines[i], "=", 
                        CSLT_STRIPLEADSPACES | CSLT_STRIPENDSPACES);
            if( ( pszConfigSingleLine[0] != NULL) && (pszConfigSingleLine[1] != NULL ) )
            {
                if( strcmp(pszConfigSingleLine[0], "Driver") == 0)
                {
                    pszDriver = strdup(pszConfigSingleLine[1]);
                }
                else if( strcmp(pszConfigSingleLine[0], "Rule") == 0)
                {
                    /* new rule */
                    stretchList.num_stretches++;
                    stretchList.stretches = (struct stretch*)CPLRealloc(stretchList.stretches, 
                                    sizeof(struct stretch) * stretchList.num_stretches);
                    if( !stretch_from_string(&stretchList.stretches[stretchList.num_stretches-1], pszConfigSingleLine[1]) )
                    {
                        exit(1);
                    }
                }
            }
            CSLDestroy(pszConfigSingleLine);

            i++;
        }

        CSLDestroy(pszConfigLines);
    }
    else
    {
        /*fprintf(stderr, "no config file %s\n", pszConfigFile);*/
    }

    CPLFree(pszConfigFile);

    /* if no rules were read in then use pszDefaultStretchRules */
    if( stretchList.num_stretches == 0 )
    {
        /*fprintf(stderr, "No stretches supplied, using default\n");*/
        i = 0;
        while(pszDefaultStretchRules[i] != NULL)
        {
            stretchList.num_stretches++;
            stretchList.stretches = (struct stretch*)CPLRealloc(stretchList.stretches, 
                            sizeof(struct stretch) * stretchList.num_stretches);
            if( !stretch_from_string(&stretchList.stretches[stretchList.num_stretches-1], pszDefaultStretchRules[i]) )
            {
                exit(1);
            }
            i++;
        }
    }

/* -------------------------------------------------------------------- */
/*      Handle command line arguments.                                  */
/* -------------------------------------------------------------------- */
    for( i = 1; i < argc; i++ )
    {
        if( strcmp(argv[i], "--printdrivers" ) == 0 )
        {
            printDrivers();
            exit(1);
        }
        else if( strcmp( argv[i], "--driver" ) == 0 )
        {
            if( i+1 < argc )
            {
                /* take copy since we free below */
                /* be consistent with the config file approach above */
                pszDriver = strdup(argv[i+1]);
                i++;
            }
            else
            {
                fprintf(stderr, "Must specify driver name\n");
                printUsage();
                exit(1);
            }
        }
        else if( strcmp( argv[i], "--stretch" ) == 0 )
        {
            if( i+1 < argc )
            {
                pCmdStretch = (struct stretch*)CPLCalloc(1, sizeof(struct stretch));
                if( !stretchpart_from_string(pCmdStretch, argv[i+1]) )
                {
                    CPLFree(pCmdStretch);
                    /* error already printed by stretchpart_from_string */
                    exit(1);
                }
                i++;
            }
            else
            {
                fprintf(stderr, "Must specify stretch string\n");
                printUsage();
                exit(1);
            }
        }
        else if( strcmp( argv[i], "--geolink") == 0 )
        {
            if( i+1 < argc )
            {
                pszGeolinkFile = argv[i+1];
                i++;
            }
            else
            {
                fprintf(stderr, "Must specify geolink file\n");
                printUsage();
                exit(1);
            }
        }
        else if( (strcmp( argv[i], "-h" ) == 0) || 
                (strcmp( argv[i], "--help" ) == 0) )
        {
            printUsage();
            exit(1);
        }
        else if( argv[i][0] == '-' )
        {
            fprintf(stderr, "Option %s incomplete, or not recognised.\n\n", 
                    argv[i] );
            printUsage();
            exit(1);
        }
        else
        {
            /* a filename */
            if( pszFileName != NULL )
            {
                fprintf(stderr, "only one filename can be specified\n");
                printUsage();
                exit(1);
            }
            pszFileName = argv[i];

            reload = 1;
        }
    }
    
    if( pszFileName == NULL )
    {
        printf( "filename(s) not specified\n" );
        printUsage();
        exit(1);
    }
    
    /* Initialise libcaca */
    cv = caca_create_canvas(0, 0);
    if(!cv)
    {
        fprintf(stderr, "Unable to initialise libcaca\n");
        return 1;
    }
    
    /* If they have specified driver, use that */
    if( pszDriver != NULL )
    {
        dp = caca_create_display_with_driver(cv, pszDriver);
        if(!dp)
        {
            fprintf(stderr, "Unable to initialise libcaca with driver %s\n", pszDriver);
            return 1;
        }
        /* finished with it */
        CPLFree(pszDriver);
        pszDriver = NULL;
    }
    else
    {
        /* fall back on default */
        dp = caca_create_display(cv);
        if( !dp )
        {
            fprintf(stderr, "Unable to initialise libcaca\n");
            return 1;
        }
    }

    /* init GDAL */
    GDALAllRegister();

    /* Set the window title */
    caca_set_display_title(dp, "gdalcacaview");

    ww = caca_get_canvas_width(cv);
    wh = caca_get_canvas_height(cv);

    /* Fill the gamma table */
    gammatab[0] = 1.0;
    for(i = 0; i < GAMMA_MAX; i++)
        gammatab[i + 1] = gammatab[i] * GAMMA_FACTOR;

    /* Go ! */
    while(!quit)
    {
        caca_event_t ev;
        unsigned int const event_mask = CACA_EVENT_KEY_PRESS
                                      | CACA_EVENT_RESIZE
                                      | CACA_EVENT_QUIT;
        unsigned int new_status = 0, new_help = 0;
        int event;

        if(update)
            event = caca_get_event(dp, event_mask, &ev, 0);
        else if( pszGeolinkFile != NULL )
        {
            /* set timeout to 1 second */
            event = caca_get_event(dp, event_mask, &ev, GEOLINK_TIMEOUT);
            if( event == 0 )
            {
                /* timeout and we are geolinking */
                /* read file */
                FILE *fp = fopen(pszGeolinkFile, "r");
                if( fp != NULL )
                {
                    double dX, dY, dMetersPerCell;
                    unsigned long long currpid, pid;
#if !defined(_WIN32) || defined(__CYGWIN__) 
                    currpid = getpid();
#else
                    currpid = GetCurrentProcessId();
#endif
                    fscanf(fp, "%llu %lf %lf %lf\n", &pid, &dX, &dY, &dMetersPerCell);
                    if( (currpid != pid ) && (dX != dispExtent.dCentreX) && (dY != dispExtent.dCentreY)
                            && (dMetersPerCell != dispExtent.dMetersPerCell) )
                    {
                        /* don't bother with our own location */
                        dispExtent.dCentreX = dX;
                        dispExtent.dCentreY = dY;
                        dispExtent.dMetersPerCell = dMetersPerCell;
                        rezoom = 1;
                        update = 1;
                    }
                    fclose(fp);
                }
            }
        }
        else
        {
            event = caca_get_event(dp, event_mask, &ev, -1);
        }

        while(event)
        {
            if(caca_get_event_type(&ev) & CACA_EVENT_KEY_PRESS)
            {
                int ch = caca_get_event_key_ch(&ev);
                switch(ch)
            {
            case 'd':
                dither_algorithm++;
                if(algos[dither_algorithm * 2] == NULL)
                    dither_algorithm = 0;
                caca_set_dither_algorithm(im->dither,
                                           algos[dither_algorithm * 2]);
                new_status = STATUS_DITHERING;
                update = 1;
                break;
            case 'D':
                dither_algorithm--;
                if(dither_algorithm < 0)
                    while(algos[dither_algorithm * 2 + 2] != NULL)
                        dither_algorithm++;
                caca_set_dither_algorithm(im->dither,
                                           algos[dither_algorithm * 2]);
                new_status = STATUS_DITHERING;
                update = 1;
                break;
            case '+':
                if(!rezoom)
                {
                    dispExtent.dMetersPerCell *= ZOOM_IN_FACTOR;
                    update = 1;
                    rezoom = 1;
                }
                break;
            case '-':
                if(!rezoom)
                {
                    dispExtent.dMetersPerCell *= ZOOM_OUT_FACTOR;
                    update = 1;
                    rezoom = 1;
                }
                break;
            case 'G':
                update = 1;
                set_gamma(g + 1);
                break;
            case 'g':
                update = 1;
                set_gamma(g - 1);
                break;
            case 'x':
            case 'X':
                if(!rezoom)
                {
                    update = 1;
                    dispExtent.dCentreX = gdalfile.fullExtent.dCentreX;
                    dispExtent.dCentreY = gdalfile.fullExtent.dCentreY;
                    dispExtent.dMetersPerCell = gdalfile.fullExtent.dMetersPerCell;
                    rezoom = 1;
                    set_gamma(0);
                }
                break;
            case 'k':
            case 'K':
            case CACA_KEY_UP:
                if(!rezoom)
                {
                    dispExtent.dCentreY += ((wh * PAN_STEP) * dispExtent.dMetersPerCell);
                    rezoom = 1;
                    update = 1;
                }
                break;
            case 'j':
            case 'J':
            case CACA_KEY_DOWN:
                if(!rezoom)
                {
                    dispExtent.dCentreY -= ((wh * PAN_STEP) * dispExtent.dMetersPerCell);
                    rezoom = 1;
                    update = 1;
                }
                break;
            case 'h':
            case 'H':
            case CACA_KEY_LEFT:
                if(!rezoom)
                {
                    dispExtent.dCentreX -= ((ww * PAN_STEP) * dispExtent.dMetersPerCell);
                    rezoom = 1;
                    update = 1;
                }
                break;
            case 'l':
            case 'L':
            case CACA_KEY_RIGHT:
                if(!rezoom)
                {
                    dispExtent.dCentreX += ((ww * PAN_STEP) * dispExtent.dMetersPerCell);
                    rezoom = 1;
                    update = 1;
                }
                break;
            case '?':
                new_help = !help;
                update = 1;
                break;
            case 'q':
            case 'Q':
            case CACA_KEY_ESCAPE:
                quit = 1;
                break;
            }
            }
            else if(caca_get_event_type(&ev) == CACA_EVENT_RESIZE)
            {
                caca_refresh_display(dp);
                ww = caca_get_event_resize_width(&ev);
                wh = caca_get_event_resize_height(&ev);
                update = 1;
                rezoom = 1;
            }
            else if(caca_get_event_type(&ev) & CACA_EVENT_QUIT)
                quit = 1;

            if(status || new_status)
                status = new_status;

            if(help || new_help)
                help = new_help;

            event = caca_get_event(dp, CACA_EVENT_KEY_PRESS, &ev, 0);
        }

        if(reload)
        {
            char *buffer;
            int len = strlen(" Loading `%s'... ") + strlen(pszFileName);

            if(len < ww + 1)
                len = ww + 1;

            buffer = CPLMalloc(len);

            sprintf(buffer, " Loading `%s'... ", pszFileName);
            buffer[ww] = '\0';
            caca_set_color_ansi(cv, CACA_WHITE, CACA_BLUE);
            caca_put_str(cv, (ww - strlen(buffer)) / 2, wh / 2, buffer);
            caca_refresh_display(dp);
            ww = caca_get_canvas_width(cv);
            wh = caca_get_canvas_height(cv);

            if(gdalfile.ds)
            {
                gdal_close_file(&gdalfile);
            }
            if(im)
            {
                gdal_unload_image(im);
                im = NULL;
            }

            if( gdal_open_file(pszFileName, &gdalfile, &stretchList, pCmdStretch) )
            {
                dispExtent.dCentreX = gdalfile.fullExtent.dCentreX;
                dispExtent.dCentreY = gdalfile.fullExtent.dCentreY;
                dispExtent.dMetersPerCell = gdalfile.fullExtent.dMetersPerCell;
                im = gdal_load_image(&gdalfile, &dispExtent);
            }

            reload = 0;
            rezoom = 0;

            /* Reset image-specific runtime variables */
            update = 1;
            set_gamma(0);

            CPLFree(buffer);
        }

        if(rezoom)
        {
            if(im)
            {
                gdal_unload_image(im);
                im = NULL;
            }

            im = gdal_load_image(&gdalfile, &dispExtent);

            /* Write out new information to geolink file if required */
            if( pszGeolinkFile != NULL )
            {
                FILE *fp = fopen(pszGeolinkFile, "w");
                if( fp != NULL )
                {
                    unsigned long long pid;
#if !defined(_WIN32) || defined(__CYGWIN__) 
                    pid = getpid();
#else
                    pid = GetCurrentProcessId();
#endif
                    fprintf(fp, "%llu %f %f %f\n", pid, dispExtent.dCentreX, 
                        dispExtent.dCentreY, dispExtent.dMetersPerCell);
                    fclose(fp);
                }
            }

            rezoom = 0;
        }

        caca_set_color_ansi(cv, CACA_WHITE, CACA_BLACK);
        caca_clear_canvas(cv);

        if(!im)
        {
            char *buffer;
            char *error = " Error loading `%s'. ";
            int len = strlen(error) + strlen(pszFileName);
            
            if( strlen( szGDALMessages ) != 0 )
            {
                /* if there was a message returned use it */
                /* no extra formatting required */
                error = szGDALMessages;
                len = strlen(error);
            }

            if(len < ww + 1)
                len = ww + 1;

            buffer = CPLMalloc(len);

            sprintf(buffer, error, pszFileName);
            buffer[ww] = '\0';
            caca_set_color_ansi(cv, CACA_WHITE, CACA_BLUE);
            caca_put_str(cv, (ww - strlen(buffer)) / 2, wh / 2, buffer);
            CPLFree(buffer);
        }
        else
        {
            caca_dither_bitmap(cv, 0, 1, ww, wh - 3, im->dither, im->pixels);
        }

        print_status();

        caca_set_color_ansi(cv, CACA_LIGHTGRAY, CACA_BLACK);
        switch(status)
        {
            case STATUS_DITHERING:
                caca_printf(cv, 0, wh - 1, "Dithering: %s",
                             caca_get_dither_algorithm(im->dither));
                break;
        }

        if(help)
        {
            print_help(ww - 26, 2);
        }

        caca_refresh_display(dp);
        update = 0;
    }

    /* Clean up */
    if(im)
        gdal_unload_image(im);
    if(gdalfile.ds)
        gdal_close_file(&gdalfile);
    if(pszStretchStatusString)
        CPLFree(pszStretchStatusString);
    if(pCmdStretch)
        CPLFree(pCmdStretch);
    caca_free_display(dp);
    caca_free_canvas(cv);

    return 0;
}

static void print_status(void)
{
    caca_set_color_ansi(cv, CACA_WHITE, CACA_BLUE);
    caca_draw_line(cv, 0, 0, ww - 1, 0, ' ');
    caca_draw_line(cv, 0, wh - 2, ww - 1, wh - 2, '-');
    caca_put_str(cv, 0, 0, "q:Quit +-x:Zoom  gG:Gamma  "
                            "hjkl:Move  d:Dither");
    caca_put_str(cv, ww - strlen("?:Help"), 0, "?:Help");
    caca_printf(cv, ww - 30, wh - 2, "(gamma: %#.3g)", GAMMA(g));
/*    caca_printf(cv, ww - 14, wh - 2, "(zoom: %s%i)", zoom > 0 ? "+" : "", zoom);*/

    if( pszStretchStatusString != NULL )
    {
        caca_put_str(cv, 10, wh - 2, pszStretchStatusString);
    }
    else
    {
        caca_put_str(cv, 10, wh - 2, "No Stretch");
    }

    caca_set_color_ansi(cv, CACA_LIGHTGRAY, CACA_BLACK);
    caca_draw_line(cv, 0, wh - 1, ww - 1, wh - 1, ' ');
}

static void print_help(int x, int y)
{
    static char const *help[] =
    {
        " +: zoom in              ",
        " -: zoom out             ",
        " g: decrease gamma       ",
        " G: increase gamma       ",
        " x: reset zoom and gamma ",
        " ----------------------- ",
        " hjkl: move view         ",
        " arrows: move view       ",
        " ----------------------- ",
        " d: dithering method     ",
        " ----------------------- ",
        " ?: help                 ",
        " q: quit                 ",
        NULL
    };

    int i;

    caca_set_color_ansi(cv, CACA_WHITE, CACA_BLUE);

    for(i = 0; help[i]; i++)
        caca_put_str(cv, x, y + i, help[i]);
}

static void set_gamma(int new_gamma)
{
    if(!im)
        return;

    g = new_gamma;

    if(g > GAMMA_MAX) g = GAMMA_MAX;
    if(g < -GAMMA_MAX) g = -GAMMA_MAX;

    caca_set_dither_gamma(im->dither,
                           (g < 0) ? 1.0 / gammatab[-g] : gammatab[g]);
}

static void draw_checkers(int x, int y, int w, int h)
{
    int xn, yn;

    if(x + w > (int)caca_get_canvas_width(cv))
        w = caca_get_canvas_width(cv) - x;
    if(y + h > (int)caca_get_canvas_height(cv))
        h = caca_get_canvas_height(cv) - y;

    for(yn = y > 0 ? y : 0; yn < y + h; yn++)
        for(xn = x > 0 ? x : 0; xn < x + w; xn++)
    {
        if((((xn - x) / 5) ^ ((yn - y) / 3)) & 1)
            caca_set_color_ansi(cv, CACA_LIGHTGRAY, CACA_DARKGRAY);
        else
            caca_set_color_ansi(cv, CACA_DARKGRAY, CACA_LIGHTGRAY);
        caca_put_char(cv, xn, yn, ' ');
    }
}

int gdal_get_best_overview(GDALDatasetH ds, struct extent *extent)
{
    /* look for a suitable overview using band 1 */
    /* return 0 to use full res, or overviewIndex+1 */
    GDALRasterBandH bandh = GDALGetRasterBand(ds, 1);
    int count, nOverviews, nFactor, nBestIndex;
    double adfTransform[6], dBestPixelsPerCell, dPixelsPerCell;

    nOverviews = GDALGetOverviewCount(bandh);
    if( nOverviews == 0 )
    {
        /* no overviews - use full resolution */
        return 0;
    }

    if( GDALGetGeoTransform(ds, adfTransform) != CE_None )
    {
        /* Should raise an error here? */
        return 0;
    }

    /* full resolution */
    dBestPixelsPerCell = extent->dMetersPerCell / adfTransform[1];
    nBestIndex = 0; /* 0 is full res, otherwise overview+1 */

    /* now overviews */
    /* we want something close to PIX_PER_CELL but greater */
    for( count = 0; count < nOverviews; count++ )
    {
        GDALRasterBandH ovh = GDALGetOverview(bandh, count);
        nFactor = GDALGetRasterXSize(ds) / GDALGetRasterBandXSize(ovh);
        dPixelsPerCell = extent->dMetersPerCell / (adfTransform[1] * nFactor);
        if( ( dPixelsPerCell > PIX_PER_CELL ) && 
            ( ( dPixelsPerCell - PIX_PER_CELL ) < ( dBestPixelsPerCell - PIX_PER_CELL ) ) )
        {
            dBestPixelsPerCell = dPixelsPerCell;
            nBestIndex = count + 1;
        }
    }

    return nBestIndex;
}

void gdal_dump_image(const char *pszFilename,int depth, struct image *im)
{
    /* debugging routine for dumping contents of im->pixels to text file */
    int xcount, ycount;
    FILE *fh = fopen(pszFilename,"w");
    if( fh != NULL )
    {
        fprintf( fh, "width = %d height = %d\n", im->w, im->h );
        
        for( ycount = 0; ycount < im->h; ycount++ )
        {
            for( xcount = 0; xcount < im->w*depth; xcount++ )
            {
                fprintf( fh, "%d,", (int)im->pixels[ycount*im->w*depth+xcount] );
            }
            fprintf( fh, "\n" );
        }
        fclose(fh);
    }
}

/* stretches data to range 0-255 based on the current stretch */
/* TODO: cache this information between stretches */
int do_stretch(float *pBuffer, GDALRasterBandH bandh, int size, struct stretch *stretch)
{
    int n, nbins, *pHisto = NULL;
    const char *pszStdDev, *pszMean, *pszMin, *pszMax;
    double stddev, mean, dMin, dMax, dStep;
    double dSumHisto, dSumVals, dBandLower, dBandUpper, dStretchMin, dStretchMax;
    float dVal;
    GDALRasterAttributeTableH rath;

        pszMin = GDALGetMetadataItem(bandh,"STATISTICS_MINIMUM",NULL);
        pszMax = GDALGetMetadataItem(bandh,"STATISTICS_MAXIMUM",NULL);
        if( (pszMin == NULL) || (pszMax == NULL))
        {
            snprintf( szGDALMessages, GDAL_ERROR_SIZE, "Statistics not available. Run gdalcalcstats first" );
            return -1;
        }

        dMin = atof(pszMin);
        dMax = atof(pszMax);

      /* Get the stats for the Band */
      if( stretch->stretchmode == VIEWER_STRETCHMODE_STDDEV)
      {
          pszStdDev = GDALGetMetadataItem(bandh,"STATISTICS_STDDEV",NULL);
          pszMean = GDALGetMetadataItem(bandh,"STATISTICS_MEAN",NULL);
          if( ( pszStdDev == NULL ) || ( pszMean == NULL ) )
          {
            snprintf( szGDALMessages, GDAL_ERROR_SIZE, "Statistics not available. Run gdalcalcstats first" );
            return -1;
          }
      
          stddev = atof( pszStdDev );
          mean = atof( pszMean );
      
          /* now apply the standard deviation stretch */
          for( n = 0; n < size; n++)
          {
            if((pBuffer[n] <= dMin) || (pBuffer[n] == 0))
                pBuffer[n] = 0;
            else if(pBuffer[n] >= dMax)
                pBuffer[n] = 255;
            else
            {
                dVal = ((pBuffer[n] - mean + stddev * stretch->stretchparam[0]) * 255)/(stddev * 2 *stretch->stretchparam[0]);
                if( dVal < 0 )
                    pBuffer[n] = 0;
                else if( dVal > 255 )
                    pBuffer[n] = 255;
                else
                    pBuffer[n] = dVal;
            }
          }
      }
      else if( stretch->stretchmode == VIEWER_STRETCHMODE_HIST)
      {
        /* Read the histogram */
        rath = GDALGetDefaultRAT(bandh);
        if(rath == NULL)
        {
            snprintf( szGDALMessages, GDAL_ERROR_SIZE, "Histogram not available. Run gdalcalcstats first" );
            return -1;
        }

        nbins = GDALRATGetRowCount(rath);
        for( n = 0; n < GDALRATGetColumnCount(rath); n++)
        {
            if( GDALRATGetUsageOfCol(rath, n) == GFU_PixelCount)
            {
                pHisto = (int*)CPLMalloc(nbins * sizeof(int));
                GDALRATValuesIOAsInteger(rath, GF_Read, n, 0, nbins, pHisto);
            }
        }
        
        if(pHisto == NULL)
        {
            snprintf( szGDALMessages, GDAL_ERROR_SIZE, "Histogram not available. Run gdalcalcstats first" );
            return -1;
        }

        dSumHisto = 0;
        for( n = 0; n < nbins; n++)
            dSumHisto += pHisto[n];
        
        dBandLower = dSumHisto * stretch->stretchparam[0];
        dBandUpper = dSumHisto * stretch->stretchparam[1];

        dStretchMin = dMin;
        dStretchMax = dMax;
        dSumVals = 0;
        for( n = 0; n < nbins; n++ )
        {
            dSumVals += pHisto[n];
            if( dSumVals > dBandLower )
            {
                dStretchMin = dMin + ((dMax - dMin) * (n / nbins));
                break;
            }
        }

        dSumVals = 0;
        for( n = nbins; n >= 0; n-- )
        {
            dSumVals += pHisto[n];
            if( dSumVals > dBandUpper )
            {
                dStretchMax = dMax + ((nbins - n - 1) / nbins);
                break;
            }
        }

        CPLFree(pHisto);

        dStep = (dStretchMax - dStretchMin) / 255.0;
        for( n = 0; n < size; n++)
        {
            if(pBuffer[n] <= dStretchMin)
                pBuffer[n] = 0;
            else if(pBuffer[n] >= dStretchMax)
                pBuffer[n] = 255;
            else
            {
                dVal = (pBuffer[n] - dStretchMin) * dStep;
                if( dVal < 0 )
                    pBuffer[n] = 0;
                else if( dVal > 255 )
                    pBuffer[n] = 255;
                else
                    pBuffer[n] = dVal;
            }
        }

      }
      else if( stretch->stretchmode == VIEWER_STRETCHMODE_LINEAR )
      {
        dStep = (dMax - dMin) / 255.0;

        /* linear stretch */
        for( n = 0; n < size; n++)
        {
            if(pBuffer[n] <= dMin)
                pBuffer[n] = 0;
            else if(pBuffer[n] >= dMax)
                pBuffer[n] = 255;
            else
            {
                dVal = (pBuffer[n] - dMin) * dStep;
                if( dVal < 0 )
                    pBuffer[n] = 0;
                else if( dVal > 255 )
                    pBuffer[n] = 255;
                else
                    pBuffer[n] = dVal;
            }
        }
      }
      else if( stretch->stretchmode != VIEWER_STRETCHMODE_NONE)
      {
        snprintf( szGDALMessages, GDAL_ERROR_SIZE, "stretch not currently supported" );
        return -1;
      }

    return 1;
}

/* Info for passing to GDALRasterIO */
struct readInfo
{
    int nXOff, nYOff, nXSize, nYSize;
    int nDataOffset;
    int nBufXSize, nBufYSize;
};

int prepare_for_reading(int dataWidth, int dataHeight, GDALDatasetH ds, GDALRasterBandH ovh, struct extent *extent,
            struct readInfo *info)
{
    double adfTransform[6], adfInvertTransform[6], x1, y1, x2, y2;
    int tlx, tly, brx, bry, width, height, nFactor;
    double dTLX, dTLY, dBRX, dBRY;
    int leftExtra, rightExtra, topExtra, bottomExtra;
    int widthIn, heightIn, origWidthIn, origHeightIn;

    width = GDALGetRasterBandXSize(ovh);
    height = GDALGetRasterBandYSize(ovh);
    nFactor = GDALGetRasterXSize(ds) / width;

    dTLX = extent->dCentreX - ((ww / 2.0) * extent->dMetersPerCell);
    dBRX = extent->dCentreX + ((ww / 2.0) * extent->dMetersPerCell);
    dTLY = extent->dCentreY + ((wh / 2.0) * extent->dMetersPerCell);
    dBRY = extent->dCentreY - ((wh / 2.0) * extent->dMetersPerCell);

    /* work out where we are from extent */
    if( GDALGetGeoTransform(ds, adfTransform) != CE_None )
    {
        snprintf( szGDALMessages, GDAL_ERROR_SIZE, "Image has no geotransform" );
        return 0;
    }

    /* Adjust pixel size for overview */
    adfTransform[1] *= nFactor;
    adfTransform[5] *= nFactor;

    if( !GDALInvGeoTransform(adfTransform, adfInvertTransform) )
    {
        snprintf( szGDALMessages, GDAL_ERROR_SIZE, "Unable to invert transform" );
        return 0;
    }
    GDALApplyGeoTransform(adfInvertTransform, dTLX, dTLY, &x1, &y1);
    GDALApplyGeoTransform(adfInvertTransform, dBRX, dBRY, &x2, &y2);
    leftExtra = 0;
    rightExtra = 0;
    topExtra = 0;
    bottomExtra = 0;
    origWidthIn = round(x2 - x1);
    origHeightIn = round(y2 - y1);

    /* leftExtra etc in Units of pixels */
    if( x1 < 0 )
    {
        leftExtra = abs(x1);
        x1 = 0;
    }
    if( x2 >= width )
    {
        rightExtra = (x2 - width - 1);
        x2 = width - 1;
    }
    if( y1 < 0 )
    {
        topExtra = abs(y1);
        y1 = 0;
    }
    if( y2 >= height )
    {
        bottomExtra = (y2 - height - 1);
        y2 = height - 1;
    }
    widthIn = round(x2 - x1);
    heightIn = round(y2 - y1);
    tlx = x1;
    tly = y1;
    brx = tlx + widthIn;
    bry = tly + heightIn;

    /* leftExtra now in units of the data read */
    leftExtra = (leftExtra / (double)origWidthIn) * dataWidth;
    rightExtra = (rightExtra / (double)origWidthIn) * dataWidth;
    topExtra = (topExtra / (double)origHeightIn) * dataHeight;
    bottomExtra = (bottomExtra / (double)origHeightIn) * dataHeight;

    info->nXOff = tlx;
    info->nYOff = tly;
    info->nXSize = widthIn;
    info->nYSize = heightIn;
    info->nDataOffset = leftExtra + (topExtra * dataWidth);
    info->nBufXSize = dataWidth - leftExtra - rightExtra;
    info->nBufYSize = dataHeight - topExtra - bottomExtra;

    return 1;
}

int gdal_read_multiband(GDALDatasetH ds,struct image *im,int overviewIndex, struct stretch *stretch, struct extent *extent)
{
    int count;
    int incount, outcount;
    float *pBuffer;
    struct readInfo info;
    GDALRasterBandH ovh;
    
    /* fill in the width and height from the overview */
    GDALRasterBandH bandh = GDALGetRasterBand(ds,stretch->bands[0]);
    if( overviewIndex == 0 )
    {
        ovh = bandh;
    }
    else
    {
        ovh = GDALGetOverview(bandh,overviewIndex - 1);
    }
    im->w = PIX_PER_CELL * ww;
    im->h = PIX_PER_CELL * wh;

    if( !prepare_for_reading(im->w, im->h, ds, ovh, extent, &info) )
    {
        /* error should already be set */
        return -1;
    }
    
    im->pixels = (char*)CPLCalloc(im->w * im->h, IMG_DEPTH);
    pBuffer = (float*)CPLCalloc(im->w * im->h, sizeof(float));

    /* read in our 3 bands */    
    for( count = 0; count < 3; count++ )
    {
      /* read in band interleaved by pixel */
      /* Basically we make each line 3 times as long */
      /* the first bixel is band[0], second is band[1] and third is band[2] */
      /* and then back to band[0] etc. So we call RasterIO 3 times, one for */
      /* each band and tell it to fill in every third pixel each time */
      bandh = GDALGetRasterBand(ds,stretch->bands[count]);
      if( overviewIndex == 0 )
      {
          ovh = bandh;
      }
      else
      {
          ovh = GDALGetOverview(bandh,overviewIndex - 1);
      }

      GDALRasterIO( ovh, GF_Read, info.nXOff, info.nYOff, info.nXSize, info.nYSize,
            &pBuffer[info.nDataOffset], info.nBufXSize, info.nBufYSize,
            GDT_Float32, sizeof(float), im->w * sizeof(float) );

      if( do_stretch(pBuffer, bandh, im->w * im->h, stretch) < 0 )
      {
          CPLFree(im->pixels);
          CPLFree(pBuffer);
          return -1;
      }

      /* copy into our bil */
      outcount = count;
      for(incount = 0; incount < (im->w * im->h); incount++ )
      {
         im->pixels[outcount] = pBuffer[incount];
         outcount += IMG_DEPTH;
      }

    }


    /* Create the libcaca dither */
    im->dither = caca_create_dither(8 * IMG_DEPTH, im->w, im->h, IMG_DEPTH * im->w,
                                     RMASK, GMASK, BMASK, AMASK);
    if(!im->dither)
    {
        CPLFree(im->pixels);
        CPLFree(pBuffer);
        snprintf( szGDALMessages, GDAL_ERROR_SIZE, "Unable to create dither" );
        return -1;
    }

    CPLFree(pBuffer);
    
    return 0;
}

int gdal_read_singleband(GDALDatasetH ds,struct image *im,int overviewIndex, struct stretch *stretch, struct extent *extent)
{
    /* Read a single band image */
    int count, outcount, incount;
    GDALRasterAttributeTableH rath;
    float *pBuffer;
    int *pRed = NULL, *pGreen = NULL, *pBlue = NULL;
    GDALRATFieldUsage eUsage;
    struct readInfo info;
    GDALRasterBandH ovh;
    
    /* fill in the width and height from the overview */
    GDALRasterBandH bandh = GDALGetRasterBand(ds,stretch->bands[0]);
    if( overviewIndex == 0 )
    {
        ovh = bandh;
    }
    else
    {
        ovh = GDALGetOverview(bandh,overviewIndex - 1);
    }
    im->w = GDALGetRasterBandXSize(ovh);
    im->h = GDALGetRasterBandYSize(ovh);

    im->w = PIX_PER_CELL * ww;
    im->h = PIX_PER_CELL * wh;

    if( !prepare_for_reading(im->w, im->h, ds, ovh, extent, &info) )
    {
        /* error should already be set */
        return -1;
    }
    
    im->pixels = (char*)CPLCalloc(im->w * im->h, IMG_DEPTH);
    pBuffer = (float*)CPLCalloc(im->w * im->h, sizeof(float));

    GDALRasterIO( ovh, GF_Read, info.nXOff, info.nYOff, info.nXSize, info.nYSize,
            &pBuffer[info.nDataOffset], info.nBufXSize, info.nBufYSize,
            GDT_Float32, sizeof(float), im->w * sizeof(float) );

   if( do_stretch(pBuffer, bandh, im->w * im->h, stretch) < 0 )
   {
      CPLFree(im->pixels);
      CPLFree(pBuffer);
      return -1;
   }
    
    if( stretch->mode == VIEWER_MODE_COLORTABLE )
    {
        /* Need to grab the RAT and read the colour columns */
        /* can't do the caca_set_dither_palette call since that is limited to 256 classes */
        rath = GDALGetDefaultRAT(bandh);
        if( rath == NULL )
        {
            CPLFree(im->pixels);
            CPLFree(pBuffer);
            snprintf( szGDALMessages, GDAL_ERROR_SIZE, "Unable to Raster Attribute Table" );
            return -1;
        }

        incount = GDALRATGetRowCount(rath);
        for( count = 0; count < GDALRATGetColumnCount(rath); count++)
        {
            eUsage = GDALRATGetUsageOfCol(rath, count);
            if( eUsage == GFU_Red )
            {
                pRed = (int*)CPLMalloc(incount * sizeof(int));
                GDALRATValuesIOAsInteger(rath, GF_Read, count, 0, incount, pRed);
            }
            else if( eUsage == GFU_Green )
            {
                pGreen = (int*)CPLMalloc(incount * sizeof(int));
                GDALRATValuesIOAsInteger(rath, GF_Read, count, 0, incount, pGreen);
            }
            else if( eUsage == GFU_Blue )
            {
                pBlue = (int*)CPLMalloc(incount * sizeof(int));
                GDALRATValuesIOAsInteger(rath, GF_Read, count, 0, incount, pGreen);
            }
        }

        /* Did we get all the columns? */
        if( (pRed == NULL) || (pGreen == NULL) || (pBlue == NULL) )
        {
            CPLFree(im->pixels);
            CPLFree(pBuffer);
            snprintf( szGDALMessages, GDAL_ERROR_SIZE, "Unable to find Red, Green and Blue columns" );
            return -1;
        }

          /* look up the colours and put into our bil */
          outcount = 0;
          for(incount = 0; incount < (im->w * im->h); incount++ )
          {
             im->pixels[outcount] = pRed[(int)pBuffer[incount]];
             outcount++;
             im->pixels[outcount] = pGreen[(int)pBuffer[incount]];
             outcount++;
             im->pixels[outcount] = pBlue[(int)pBuffer[incount]];
             outcount++;
          }

        CPLFree(pRed);
        CPLFree(pGreen);
        CPLFree(pBlue);

    }
    else if( stretch->mode == VIEWER_MODE_GREYSCALE )
    {
      /* copy into our bil and repeat the colours */
      outcount = 0;
      for(incount = 0; incount < (im->w * im->h); incount++ )
      {
         im->pixels[outcount] = pBuffer[incount];
         outcount++;
         im->pixels[outcount] = pBuffer[incount];
         outcount++;
         im->pixels[outcount] = pBuffer[incount];
         outcount++;
      }

    }
    else
    {
        snprintf( szGDALMessages, GDAL_ERROR_SIZE, "Unsupported stretch");
        CPLFree(im->pixels);
        CPLFree(pBuffer);
        return -1;
    }

    /* Create the libcaca dither */
    im->dither = caca_create_dither(8 * IMG_DEPTH, im->w, im->h, IMG_DEPTH * im->w,
                                     RMASK, GMASK, BMASK, AMASK);
    if(!im->dither)
    {
        CPLFree(im->pixels);
        CPLFree(pBuffer);
        snprintf( szGDALMessages, GDAL_ERROR_SIZE, "Unable to create dither" );
        return -1;
    }


    CPLFree(pBuffer);
    
    return 0;
}

/* returns a newly malloced string describing the stretch */
char *get_stretch_as_string(struct stretch *stretch)
{
char szMode[GDAL_ERROR_SIZE], *pszStr;
char szStretchMode[GDAL_ERROR_SIZE];

    szMode[0] = '\0';
    if( stretch->mode == VIEWER_MODE_COLORTABLE )
        snprintf(szMode, GDAL_ERROR_SIZE, "Color Table %d", stretch->bands[0]);
    else if( stretch->mode == VIEWER_MODE_GREYSCALE )
        snprintf(szMode, GDAL_ERROR_SIZE, "GreyScale %d", stretch->bands[0]);
    else if( stretch->mode == VIEWER_MODE_RGB )
        snprintf(szMode, GDAL_ERROR_SIZE, "RGB %d %d %d", stretch->bands[0],
            stretch->bands[1], stretch->bands[2]);
    else if( stretch->mode == VIEWER_MODE_PSEUDOCOLOR )
        snprintf(szMode, GDAL_ERROR_SIZE, "PseudoColor %d", stretch->bands[0]);

    szStretchMode[0] = '\0';
    if( stretch->stretchmode == VIEWER_STRETCHMODE_NONE )
        snprintf(szStretchMode, GDAL_ERROR_SIZE, " No Stretch");
    else if( stretch->stretchmode == VIEWER_STRETCHMODE_LINEAR )
        snprintf(szStretchMode, GDAL_ERROR_SIZE, " Linear Stretch %.2f - %.2f", 
            stretch->stretchparam[0], stretch->stretchparam[1]);
    else if( stretch->stretchmode == VIEWER_STRETCHMODE_STDDEV )
        snprintf(szStretchMode, GDAL_ERROR_SIZE, " Standard Deviation %.2f", stretch->stretchparam[0]);
    else if( stretch->stretchmode == VIEWER_STRETCHMODE_HIST )
        snprintf(szStretchMode, GDAL_ERROR_SIZE, " Histogram Stretch %.2f - %.2f", 
            stretch->stretchparam[0], stretch->stretchparam[1]);

    pszStr = CPLMalloc(strlen(szMode) + strlen(szStretchMode) + 1);
    strcpy(pszStr, szMode);
    strcat(pszStr, szStretchMode);
    return pszStr;
}

/* pCmdStretch non-NULL if they have passed in a stretch on the command line */
int gdal_open_file(char const *pszFile, struct gdalFile *file, struct stretchlist *stretchList, 
                    struct stretch *pCmdStretch)
{
    int xsize, ysize;

    /* reset error message buffer */
    szGDALMessages[0] = '\0';

    /* attempt to open with GDAL */
    file->ds = GDALOpen(pszFile, GA_ReadOnly);
    if( file->ds == NULL )
    {
      CPLFree(im);
      snprintf( szGDALMessages, GDAL_ERROR_SIZE, "Could not open %s with GDAL", pszFile );
      return 0;
    }

    /* get the stretch */
    if( pCmdStretch != NULL )
    {
        file->stretch = pCmdStretch;
    }
    else
    {
        file->stretch = get_stretch_for_gdal(stretchList, file->ds);
        if( file->stretch == NULL )
        {
            gdal_close_file(file);
            snprintf( szGDALMessages, GDAL_ERROR_SIZE, "Could not find stretch to use for %s", pszFile);
            return 0;
        }
    }

    /* status string */
    if(pszStretchStatusString != NULL)
    {
        CPLFree(pszStretchStatusString);
    }
    pszStretchStatusString = get_stretch_as_string(file->stretch);

    /* get full extent */
    if( GDALGetGeoTransform(file->ds, file->adfTransform) != CE_None )
    {
        gdal_close_file(file);
        snprintf( szGDALMessages, GDAL_ERROR_SIZE, "No Geo Transform");
        return 0;
    }

    xsize = GDALGetRasterXSize(file->ds);
    ysize = GDALGetRasterYSize(file->ds);

    file->fullExtent.dCentreX = file->adfTransform[0] + file->adfTransform[1] * (xsize / 2);
    file->fullExtent.dCentreY = file->adfTransform[3] + file->adfTransform[5] * (ysize / 2);
    file->fullExtent.dMetersPerCell = MAX((file->adfTransform[1] * xsize) / ww,
                    (-file->adfTransform[5] * ysize) / wh);

    return 1;
}

void gdal_close_file(struct gdalFile* file)
{
    GDALClose(file->ds);
    file->ds = NULL;
}

extern struct image * gdal_load_image(struct gdalFile *file, struct extent *extent)
{
    struct image * im;
    int overviewIndex;

    /* reset error message buffer */
    szGDALMessages[0] = '\0';

    /* create our image structure */
    im = CPLMalloc(sizeof(struct image));
    
    /* Find the best overview level to use */
    overviewIndex = gdal_get_best_overview(file->ds, extent);
   
    if( file->stretch->mode == VIEWER_MODE_RGB )
    {
      if( gdal_read_multiband(file->ds,im,overviewIndex, file->stretch, extent) == -1 )
      {
        /* trap error. Should have filled in message */
        CPLFree(im);
        gdal_close_file(file);
        return NULL;
      }
    }
    else
    {
      if( gdal_read_singleband(file->ds,im,overviewIndex, file->stretch, extent) == -1 )
      {
        /* trap error. Should have filled in message */
        CPLFree(im);
        gdal_close_file(file);
        return NULL;
      }
    }
    
    return im;
}

void gdal_unload_image(struct image * im)
{
    CPLFree(im->pixels);
    caca_free_dither(im->dither);
    CPLFree(im);
    /* reset error message buffer */
    szGDALMessages[0] = '\0';

}
