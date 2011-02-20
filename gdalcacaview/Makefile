CFLAGS=-I$(GDAL_INCLUDE_PATH) -I$(GDALHG_INCLUDE_PATH) -O2 -Wall
GDAL=-L$(GDAL_LIB_PATH) -lgdal -L$(LIBCACA_LIB_PATH) -lcaca
CFLAGS=-I$(GDAL_INCLUDE_PATH) -I$(LIBCACA_INCLUDE_PATH)
CC=gcc -O2
OUTNAME=gdalcacaview

all: gdalcacaview

.c.o:
	$(CC) $(CFLAGS) -c $<

gdalcacaview: gdalcacaview.o Makefile
	$(CC) gdalcacaview.o $(GDAL) -o $(OUTNAME)

clean:
	rm *.o $(OUTNAME)
	
