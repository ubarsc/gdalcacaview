/*
 *  cacaview      image viewer for libcaca
 *  Copyright (c) 2003-2006 Sam Hocevar <sam@hocevar.net>
 *                All Rights Reserved
 *
 *  This program is free software. It comes without any warranty, to
 *  the extent permitted by applicable law. You can redistribute it
 *  and/or modify it under the terms of the Do What The Fuck You Want
 *  To Public License, Version 2, as published by Sam Hocevar. See
 *  http://sam.zoy.org/wtfpl/COPYING for more details.
 */

/*#include "config.h"*/

#if !defined(__KERNEL__)
#   include <stdio.h>
#   include <string.h>
#   include <stdlib.h>
#endif

#if defined(HAVE_SLEEP)
#   include <windows.h>
#endif

#if !defined(_WIN32) || defined(__CYGWIN__) 
#include <sys/types.h>
#include <pwd.h>
#endif

#include "caca.h"
#include "gdal.h"
#include "cpl_string.h"

/* Local macros */
#define MODE_IMAGE 1
#define MODE_FILES 2

#define STATUS_DITHERING 1
#define STATUS_ANTIALIASING 2
#define STATUS_BACKGROUND 3

#define ZOOM_FACTOR 1.08f
#define ZOOM_MAX 70
#define GAMMA_FACTOR 1.04f
#define GAMMA_MAX 100
#define GAMMA(g) (((g) < 0) ? 1.0 / gammatab[-(g)] : gammatab[(g)])
#define PAD_STEP 0.15

/* libcaca/libcaca contexts */
caca_canvas_t *cv; caca_display_t *dp;

/* Area for printing GDAL error messages */
#define GDAL_ERROR_SIZE 1024
char szGDALMessages[GDAL_ERROR_SIZE];

/* Default stretch rules */
char *pszDefaultStretchRules[] = {"equal,1,1,colortable,none,,1",
"equal,1,-1,greyscale,none,,1",
"equal,2,-1,greyscale,none,,1",
"equal,3,-1,rgb,none,,1|2|3",
"less,6,-1,rgb,stddev,2.0,4|3|2",
"greater,5,-1,rgb,stddev,2.0,5|4|2", NULL};


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

/* Local functions */
static void print_status(void);
static void print_help(int, int);
static void set_zoom(int);
static void set_gamma(int);
static void draw_checkers(int, int, int, int);

extern struct image * gdal_load_image(char const *, struct stretchlist *);
extern void gdal_unload_image(struct image *);

/* Local variables */
struct image *im = NULL;

float zoomtab[ZOOM_MAX + 1];
float gammatab[GAMMA_MAX + 1];
float xfactor = 1.0, yfactor = 1.0, dx = 0.5, dy = 0.5;
int zoom = 0, g = 0, fullscreen = 0, mode, ww, wh;
char *pszStretchStatusString = NULL;

int stretch_from_string(struct stretch *newStretch, const char *pszString)
{
char **pszTokens;
char **pszExtraTokens;
char *pszTmp;
int n = 0;
int i;

    pszTokens = CSLTokenizeString2(pszString, ",", 
        CSLT_STRIPLEADSPACES | CSLT_STRIPENDSPACES | CSLT_ALLOWEMPTYTOKENS);

    pszTmp = pszTokens[n];
    if( pszTmp == NULL )
    {
        fprintf(stderr, "Missing value in rule string\n");
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
        fprintf(stderr, "Missing value in rule string\n");
        CSLDestroy(pszTokens);
        return 0;
    }
    newStretch->value = atol(pszTmp);    

    n++;
    pszTmp = pszTokens[n];
    if( pszTmp == NULL )
    {
        fprintf(stderr, "Missing value in rule string\n");
        CSLDestroy(pszTokens);
        return 0;
    }
    newStretch->ctband = atol(pszTmp);

    n++;
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
        fprintf(stderr, "Unable to understand mode %s\n", pszTmp);
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
    printf("gdalcacaview [options] filename1 [filename2...]\n\n");

    printf("where options is one of:\n"); 
    printf(" --printdrivers\tPrint list of available drivers and exit\n");
    printf(" --driver DRIVER\tUse the specified driver. If not given, uses default\n");
    printf("and filename(s) are GDAL supported datasets.\n");
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
    int reload = 0;

    char **list = NULL;
    int current = 0, items = 0, opts = 1;
    int i;
    char *pszDriver = NULL;
    char *pszConfigFile = NULL, *pszHomeDir = NULL;
    char **pszConfigLines, **pszConfigSingleLine;
    struct stretchlist stretchList;

/* -------------------------------------------------------------------- */
/*      Read config file if it exists                                   */
/* -------------------------------------------------------------------- */
#if defined(_WIN32) && !defined(__CYGWIN__) 
    pszHomeDir = getenv("USERPROFILE");
    if( pszHomeDir == NULL )
    {
        /* '.gcv' plus seperating slash plus terminating null */
        pszConfigFile = malloc(strlen(pszHomeDir) + 6);
        /* copy in the path */
        strcpy(pszConfigFile, pszHomeDir);
        
    }
    else
    {
        i = strlen(getenv("HOMEDRIVE")) + 1 + strlen(getenv("HOMEPATH")) + 6;
        pszConfigFile = malloc(i);
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
    pszConfigFile = malloc(strlen(pszHomeDir) + 6);
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
                    stretchList.stretches = (struct stretch*)realloc(stretchList.stretches, 
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

    free(pszConfigFile);

    /* if no rules were read in then use pszDefaultStretchRules */
    if( stretchList.num_stretches == 0 )
    {
        /*fprintf(stderr, "No stretches supplied, using default\n");*/
        i = 0;
        while(pszDefaultStretchRules[i] != NULL)
        {
            stretchList.num_stretches++;
            stretchList.stretches = (struct stretch*)realloc(stretchList.stretches, 
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
                printf("Must specify driver name\n");
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
            printf( "Option %s incomplete, or not recognised.\n\n", 
                    argv[i] );
            printUsage();
            exit(1);
        }
        else
        {
            /* add to our list of filenames */
            if(items)
                list = realloc(list, (items + 1) * sizeof(char *));
            else
                list = malloc(sizeof(char *));
            list[items] = argv[i];
            items++;

            reload = 1;
        }
    }
    
    if( items == 0 )
    {
        printf( "filename(s) not specified\n" );
        printUsage();
        exit(1);
    }
    
    /*printf( "driver = %s\n", pszDriver);*/

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
        free(pszDriver);
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

    /* Fill the zoom table */
    zoomtab[0] = 1.0;
    for(i = 0; i < ZOOM_MAX; i++)
        zoomtab[i + 1] = zoomtab[i] * ZOOM_FACTOR;

    /* Fill the gamma table */
    gammatab[0] = 1.0;
    for(i = 0; i < GAMMA_MAX; i++)
        gammatab[i + 1] = gammatab[i] * GAMMA_FACTOR;

    /* Load items into playlist */
    
    /*for(i = 1; i < argc; i++)*/
/*    {*/
        /* Skip options except after `--' */
/*        if(opts && argv[i][0] == '-')*/
/*        {*/
/*            if(argv[i][1] == '-' && argv[i][2] == '\0')*/
/*                opts = 0;*/
/*            continue;*/
/*        }*/

        /* Add argv[i] to the list */
/*        if(items)*/
/*            list = realloc(list, (items + 1) * sizeof(char *));*/
/*        else*/
/*            list = malloc(sizeof(char *));*/
/*        list[items] = argv[i];*/
/*        items++;*/

/*        reload = 1;*/
/*    }*/

    /* Go ! */
    while(!quit)
    {
        caca_event_t ev;
        unsigned int const event_mask = CACA_EVENT_KEY_PRESS
                                      | CACA_EVENT_RESIZE
                                      | CACA_EVENT_MOUSE_PRESS
                                      | CACA_EVENT_QUIT;
        unsigned int new_status = 0, new_help = 0;
        int event;

        if(update)
            event = caca_get_event(dp, event_mask, &ev, 0);
        else
            event = caca_get_event(dp, event_mask, &ev, -1);

        while(event)
        {
            if(caca_get_event_type(&ev) & CACA_EVENT_MOUSE_PRESS)
            {
/*                if(caca_get_event_mouse_button(&ev) == 1)
                {
                    if(items) current = (current + 1) % items;
                    reload = 1;
                }
                if(caca_get_event_mouse_button(&ev) == 2)
                {
                    if(items) current = (items + current - 1) % items;
                    reload = 1;
                }*/
            }
            else if(caca_get_event_type(&ev) & CACA_EVENT_KEY_PRESS)
                switch(caca_get_event_key_ch(&ev))
            {
            case 'n':
            case 'N':
                if(items) current = (current + 1) % items;
                reload = 1;
                break;
            case 'p':
            case 'P':
                if(items) current = (items + current - 1) % items;
                reload = 1;
                break;
            case 'f':
            case 'F':
            case CACA_KEY_F11:
                fullscreen = ~fullscreen;
                update = 1;
                set_zoom(zoom);
                break;
#if 0 /* FIXME */
            case 'b':
                i = 1 + caca_get_feature(cv, CACA_BACKGROUND);
                if(i > CACA_BACKGROUND_MAX) i = CACA_BACKGROUND_MIN;
                caca_set_feature(cv, i);
                new_status = STATUS_BACKGROUND;
                update = 1;
                break;
            case 'B':
                i = -1 + caca_get_feature(cv, CACA_BACKGROUND);
                if(i < CACA_BACKGROUND_MIN) i = CACA_BACKGROUND_MAX;
                caca_set_feature(cv, i);
                new_status = STATUS_BACKGROUND;
                update = 1;
                break;
            case 'a':
                i = 1 + caca_get_feature(cv, CACA_ANTIALIASING);
                if(i > CACA_ANTIALIASING_MAX) i = CACA_ANTIALIASING_MIN;
                caca_set_feature(cv, i);
                new_status = STATUS_ANTIALIASING;
                update = 1;
                break;
            case 'A':
                i = -1 + caca_get_feature(cv, CACA_ANTIALIASING);
                if(i < CACA_ANTIALIASING_MIN) i = CACA_ANTIALIASING_MAX;
                caca_set_feature(cv, i);
                new_status = STATUS_ANTIALIASING;
                update = 1;
                break;
#endif
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
                update = 1;
                set_zoom(zoom + 1);
                break;
            case '-':
                update = 1;
                set_zoom(zoom - 1);
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
                update = 1;
                set_zoom(0);
                set_gamma(0);
                break;
            case 'k':
            case 'K':
            case CACA_KEY_UP:
                if(yfactor > 1.0) dy -= PAD_STEP / yfactor;
                if(dy < 0.0) dy = 0.0;
                update = 1;
                break;
            case 'j':
            case 'J':
            case CACA_KEY_DOWN:
                if(yfactor > 1.0) dy += PAD_STEP / yfactor;
                if(dy > 1.0) dy = 1.0;
                update = 1;
                break;
            case 'h':
            case 'H':
            case CACA_KEY_LEFT:
                if(xfactor > 1.0) dx -= PAD_STEP / xfactor;
                if(dx < 0.0) dx = 0.0;
                update = 1;
                break;
            case 'l':
            case 'L':
            case CACA_KEY_RIGHT:
                if(xfactor > 1.0) dx += PAD_STEP / xfactor;
                if(dx > 1.0) dx = 1.0;
                update = 1;
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
            else if(caca_get_event_type(&ev) == CACA_EVENT_RESIZE)
            {
                caca_refresh_display(dp);
                ww = caca_get_event_resize_width(&ev);
                wh = caca_get_event_resize_height(&ev);
                update = 1;
                set_zoom(zoom);
            }
            else if(caca_get_event_type(&ev) & CACA_EVENT_QUIT)
                quit = 1;

            if(status || new_status)
                status = new_status;

            if(help || new_help)
                help = new_help;

            event = caca_get_event(dp, CACA_EVENT_KEY_PRESS, &ev, 0);
        }

        if(items && reload)
        {
            char *buffer;
            int len = strlen(" Loading `%s'... ") + strlen(list[current]);

            if(len < ww + 1)
                len = ww + 1;

            buffer = malloc(len);

            sprintf(buffer, " Loading `%s'... ", list[current]);
            buffer[ww] = '\0';
            caca_set_color_ansi(cv, CACA_WHITE, CACA_BLUE);
            caca_put_str(cv, (ww - strlen(buffer)) / 2, wh / 2, buffer);
            caca_refresh_display(dp);
            ww = caca_get_canvas_width(cv);
            wh = caca_get_canvas_height(cv);

            if(im)
                gdal_unload_image(im);
            im = gdal_load_image(list[current], &stretchList);
            reload = 0;

            /* Reset image-specific runtime variables */
            dx = dy = 0.5;
            update = 1;
            set_zoom(0);
            set_gamma(0);

            free(buffer);
        }

        caca_set_color_ansi(cv, CACA_WHITE, CACA_BLACK);
        caca_clear_canvas(cv);

        if(!items)
        {
            caca_set_color_ansi(cv, CACA_WHITE, CACA_BLUE);
            caca_printf(cv, ww / 2 - 5, wh / 2, " No image. ");
        }
        else if(!im)
        {
#if defined(USE_IMLIB2)
#   define ERROR_STRING " Error loading `%s'. "
#else
#   define ERROR_STRING " Error loading `%s'. "
#endif
            char *buffer;
            char *error = ERROR_STRING;
            int len = strlen(ERROR_STRING) + strlen(list[current]);
            
            if( strlen( szGDALMessages ) != 0 )
            {
                /* if there was a message returned use it */
                /* no extra formatting required */
                error = szGDALMessages;
                len = strlen(error);
            }

            if(len < ww + 1)
                len = ww + 1;

            buffer = malloc(len);

            sprintf(buffer, error, list[current]);
            buffer[ww] = '\0';
            caca_set_color_ansi(cv, CACA_WHITE, CACA_BLUE);
            caca_put_str(cv, (ww - strlen(buffer)) / 2, wh / 2, buffer);
            free(buffer);
        }
        else
        {
            float xdelta, ydelta;
            int y, height;

            y = fullscreen ? 0 : 1;
            height = fullscreen ? wh : wh - 3;

            xdelta = (xfactor > 1.0) ? dx : 0.5;
            ydelta = (yfactor > 1.0) ? dy : 0.5;

            draw_checkers(ww * (1.0 - xfactor) / 2,
                          y + height * (1.0 - yfactor) / 2,
                          ww * xfactor, height * yfactor);

            caca_dither_bitmap(cv, ww * (1.0 - xfactor) * xdelta,
                            y + height * (1.0 - yfactor) * ydelta,
                            ww * xfactor + 1, height * yfactor + 1,
                            im->dither, im->pixels);
        }

        if(!fullscreen)
        {
            print_status();

            caca_set_color_ansi(cv, CACA_LIGHTGRAY, CACA_BLACK);
            switch(status)
            {
                case STATUS_DITHERING:
                    caca_printf(cv, 0, wh - 1, "Dithering: %s",
                                 caca_get_dither_algorithm(im->dither));
                    break;
#if 0 /* FIXME */
                case STATUS_ANTIALIASING:
                    caca_printf(cv, 0, wh - 1, "Antialiasing: %s",
                  caca_get_feature_name(caca_get_feature(cv, CACA_ANTIALIASING)));
                    break;
                case STATUS_BACKGROUND:
                    caca_printf(cv, 0, wh - 1, "Background: %s",
                  caca_get_feature_name(caca_get_feature(cv, CACA_BACKGROUND)));
                    break;
#endif
            }
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
    if(pszStretchStatusString)
        free(pszStretchStatusString);
    caca_free_display(dp);
    caca_free_canvas(cv);

    return 0;
}

static void print_status(void)
{
    caca_set_color_ansi(cv, CACA_WHITE, CACA_BLUE);
    caca_draw_line(cv, 0, 0, ww - 1, 0, ' ');
    caca_draw_line(cv, 0, wh - 2, ww - 1, wh - 2, '-');
    caca_put_str(cv, 0, 0, "q:Quit  np:Next/Prev  +-x:Zoom  gG:Gamma  "
                            "hjkl:Move  d:Dither  a:Antialias");
    caca_put_str(cv, ww - strlen("?:Help"), 0, "?:Help");
/*    caca_printf(cv, 3, wh - 2, "cacaview %s", PACKAGE_VERSION);*/
    caca_printf(cv, ww - 30, wh - 2, "(gamma: %#.3g)", GAMMA(g));
    caca_printf(cv, ww - 14, wh - 2, "(zoom: %s%i)", zoom > 0 ? "+" : "", zoom);

    if( pszStretchStatusString != NULL )
    {
        caca_put_str(cv, 10, wh - 2, pszStretchStatusString);
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
        " a: antialiasing method  ",
        " d: dithering method     ",
        " b: background mode      ",
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

static void set_zoom(int new_zoom)
{
    int height;

    if(!im)
        return;

    zoom = new_zoom;

    if(zoom > ZOOM_MAX) zoom = ZOOM_MAX;
    if(zoom < -ZOOM_MAX) zoom = -ZOOM_MAX;

    ww = caca_get_canvas_width(cv);
    height = fullscreen ? wh : wh - 3;

    xfactor = (zoom < 0) ? 1.0 / zoomtab[-zoom] : zoomtab[zoom];
    yfactor = xfactor * ww / height * im->h / im->w
               * caca_get_canvas_height(cv) / caca_get_canvas_width(cv)
               * caca_get_display_width(dp) / caca_get_display_height(dp);

    if(yfactor > xfactor)
    {
        float tmp = xfactor;
        xfactor = tmp * tmp / yfactor;
        yfactor = tmp;
    }
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

/* OK dodgy code below added by gillins */

#define MAX_OVERVIEW_SIZE 3000

int gdal_get_best_overview(GDALDatasetH ds)
{
    /* look for a suitable overview using band 1 */
    /* returns -1 if none found */
    int overviewIndex = -1;
    GDALRasterBandH bandh = GDALGetRasterBand(ds,1);
    int count = 0;
    while( count < GDALGetOverviewCount(bandh) )
    {
      GDALRasterBandH ovh = GDALGetOverview(bandh,count);
      if( ( GDALGetRasterBandXSize(ovh) < MAX_OVERVIEW_SIZE ) && ( GDALGetRasterBandYSize(ovh) < MAX_OVERVIEW_SIZE ) )
      {
        /* use this overview */
        overviewIndex = count;
        /* exit the loop */
        count = GDALGetOverviewCount(bandh);
      }
      count++;
    }
    return overviewIndex;
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

int do_stretch(float *pBuffer, GDALRasterBandH bandh, int size, struct stretch *stretch)
{
    int n;
    const char *pszStdDev, *pszMean;
    double stddev, mean;
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
            pBuffer[n] = ((pBuffer[n] - mean + stddev * stretch->stretchparam[0]) * 255)/(stddev * 2 *stretch->stretchparam[0]);
          }
      }
      else if( stretch->stretchmode != VIEWER_STRETCHMODE_NONE)
      {
        snprintf( szGDALMessages, GDAL_ERROR_SIZE, "stretch not currently supported" );
        return -1;
      }

    return 1;
}

int gdal_read_multiband(GDALDatasetH ds,struct image *im,int overviewIndex, struct stretch *stretch)
{
    int count;
    int incount, outcount;
    float *pBuffer;

    /* tell libcaca how we have encoded the bytes */
    /* red, then green, then blue */    
    int rmask = 0x0000ff;
    int gmask = 0x00ff00;
    int bmask = 0xff0000;
    int amask = 0x000000;
    /* Read a multiband image */
    int depth = 3; /* this is always 24 bit */
    
    /* fill in the width and height from the overview */
    GDALRasterBandH bandh = GDALGetRasterBand(ds,1);
    GDALRasterBandH ovh = GDALGetOverview(bandh,overviewIndex);
    im->w = GDALGetRasterBandXSize(ovh);
    im->h = GDALGetRasterBandYSize(ovh);
    
    im->pixels = malloc(im->w * im->h * depth);
    if(!im->pixels)
    {
        snprintf( szGDALMessages, GDAL_ERROR_SIZE, "Unable to allocate memory for %d x %d image", im->w, im->h );
        return -1;
    }

    memset(im->pixels, 0, im->w * im->h * depth);
    
    pBuffer = (float*)malloc(im->w * im->h * sizeof(float));
    if(!pBuffer)
    {
        snprintf( szGDALMessages, GDAL_ERROR_SIZE, "Unable to allocate memory for %d x %d image", im->w, im->h );
        return -1;
    }

    /* read in our 3 bands */    
    for( count = 0; count < 3; count++ )
    {
      /* read in band interleaved by pixel */
      /* Basically we make each line 3 times as long */
      /* the first bixel is band[0], second is band[1] and third is band[2] */
      /* and then back to band[0] etc. So we call RasterIO 3 times, one for */
      /* each band and tell it to fill in every third pixel each time */
      bandh = GDALGetRasterBand(ds,stretch->bands[count]);
      ovh = GDALGetOverview(bandh,overviewIndex);

      GDALRasterIO( ovh, GF_Read, 0, 0, im->w, im->h, pBuffer, im->w, im->h, GDT_Float32, sizeof(float), im->w*sizeof(float));

      if( do_stretch(pBuffer, bandh, im->w * im->h, stretch) < 0 )
      {
          free(im->pixels);
          free(pBuffer);
          return -1;
      }

      /* copy into our bil */
      outcount = count;
      for(incount = 0; incount < (im->w * im->h); incount++ )
      {
         im->pixels[outcount] = pBuffer[incount];
         outcount += depth;
      }

      /*
      char szBuffer[64];
      snprintf( szBuffer, 64, "outfile%d.txt", count );
      gdal_dump_image(szBuffer,depth,im);
      */
    }


    /* Create the libcaca dither */
    im->dither = caca_create_dither(8*depth, im->w, im->h, depth * im->w,
                                     rmask, gmask, bmask, amask);
    if(!im->dither)
    {
        free(im->pixels);
        snprintf( szGDALMessages, GDAL_ERROR_SIZE, "Unable to create dither" );
        return -1;
    }

    free(pBuffer);
    
    return 0;
}

int gdal_read_singleband(GDALDatasetH ds,struct image *im,int overviewIndex, struct stretch *stretch)
{
    /* Read a single band thematic image */
    int depth = 1; /* 8 bit */
    int count;
    const char *pszThematic;
    uint32_t red[256], green[256], blue[256], alpha[256];
    GDALColorTableH cth;
    
    /* fill in the width and height from the overview */
    GDALRasterBandH bandh = GDALGetRasterBand(ds,stretch->bands[0]);
    GDALRasterBandH ovh = GDALGetOverview(bandh,overviewIndex);
    im->w = GDALGetRasterBandXSize(ovh);
    im->h = GDALGetRasterBandYSize(ovh);
    
    /* Check we actually have thematic */
    pszThematic = GDALGetMetadataItem(bandh,"LAYER_TYPE",NULL);
    if( (pszThematic == NULL) || (strcmp( pszThematic, "thematic" ) != 0 ))
    {
        snprintf( szGDALMessages, GDAL_ERROR_SIZE, "Only support thematic single band images at the moment" );
        return -1;
    }
    
    im->pixels = malloc(im->w * im->h * depth);
    if(!im->pixels)
    {
        snprintf( szGDALMessages, GDAL_ERROR_SIZE, "Unable to allocate memory for %d x %d image", im->w, im->h );
        return -1;
    }

    memset(im->pixels, 0, im->w * im->h * depth);

    GDALRasterIO( ovh, GF_Read, 0, 0, im->w, im->h, im->pixels, im->w, im->h, GDT_Byte, depth, im->w );
    
    /*gdal_dump_image("outfile.txt",depth,im);*/

    /* Set the palette */
    memset(red,0,256 * sizeof(uint32_t));
    memset(green,0,256 * sizeof(uint32_t));
    memset(blue,0,256 * sizeof(uint32_t));
    memset(alpha,0,256 * sizeof(uint32_t));
    
    cth = GDALGetRasterColorTable(bandh);
    if( cth == NULL )
    {
        free(im->pixels);
        snprintf( szGDALMessages, GDAL_ERROR_SIZE, "Unable to read colour table" );
        return -1;
    }
    
    for( count = 0; count < GDALGetColorEntryCount(cth); count++)
    {
        const GDALColorEntry *colorentry = GDALGetColorEntry(cth,count);
        /* libcaca expects values between 0 and 4095 */
        /* GDAL gives us between 0 and 255 so we need to scale */
        double dScale = 4095.0 / 255.0;
        red[count] = ceil(colorentry->c1 * dScale);
        green[count] = ceil(colorentry->c2 * dScale);
        blue[count] = ceil(colorentry->c3 * dScale);
    }

    /* Create the libcaca dither */
    im->dither = caca_create_dither(8*depth, im->w, im->h, depth * im->w,
                                     0, 0, 0, 0);
    if(!im->dither)
    {
        free(im->pixels);
        snprintf( szGDALMessages, GDAL_ERROR_SIZE, "Unable to create dither" );
        return -1;
    }

    /* tell libcaca about the palette */
    caca_set_dither_palette(im->dither, red, green, blue, alpha);
    
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

    pszStr = malloc(strlen(szMode) + strlen(szStretchMode) + 1);
    strcpy(pszStr, szMode);
    strcat(pszStr, szStretchMode);
    return pszStr;
}

struct image * gdal_load_image(char const * name, struct stretchlist *stretchList)
{
    struct image * im;
    GDALDatasetH ds;
    struct stretch *stretch;
    int overviewIndex, nRasterCount;

    /* reset error message buffer */
    szGDALMessages[0] = '\0';

    /* create our image structure */
    im = malloc(sizeof(struct image));
    
    /* attempt to open with GDAL */
    ds = GDALOpen(name,GA_ReadOnly);
    if( ds == NULL )
    {
      free(im);
      snprintf( szGDALMessages, GDAL_ERROR_SIZE, "Could not open %s with GDAL", name );
      return NULL;
    }

    /* get the stretch */
    stretch = get_stretch_for_gdal(stretchList, ds);
    if( stretch == NULL )
    {
        free(im);
        snprintf( szGDALMessages, GDAL_ERROR_SIZE, "Could not find stretch to use for %s", name);
        return NULL;
    }

    /* status string */
    if(pszStretchStatusString != NULL)
    {
        free(pszStretchStatusString);
    }
    pszStretchStatusString = get_stretch_as_string(stretch);
    
    /* Find the best overview level to use */
    overviewIndex = gdal_get_best_overview(ds);
    if( overviewIndex == -1 )
    {
      free(im);
      GDALClose(ds);
      snprintf( szGDALMessages, GDAL_ERROR_SIZE, "%s has no overviews. Run gdalcalcstats -pyramid first", name );
      return NULL;
    }
   
    nRasterCount = GDALGetRasterCount(ds);
    if( stretch->mode == VIEWER_MODE_RGB )
    {
      if( gdal_read_multiband(ds,im,overviewIndex, stretch) == -1 )
      {
        /* trap error. Should have filled in message */
        free(im);
        GDALClose(ds);
        return NULL;
      }
    }
    else
    {
      if( gdal_read_singleband(ds,im,overviewIndex, stretch) == -1 )
      {
        /* trap error. Should have filled in message */
        free(im);
        GDALClose(ds);
        return NULL;
      }
    }
    
    GDALClose(ds);

    return im;
}

void gdal_unload_image(struct image * im)
{
    free(im->pixels);
    caca_free_dither(im->dither);
    free(im);
    /* reset error message buffer */
    szGDALMessages[0] = '\0';

    /* status string */
    if(pszStretchStatusString != NULL)
    {
        free(pszStretchStatusString);
        pszStretchStatusString = NULL;
    }
}
