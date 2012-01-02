GDAL=-L$(GDAL_LIB_PATH) $(GDAL_LINK_FLAGS) -L$(LIBCACA_LIB_PATH) -lcaca
CFLAGS=-I$(GDAL_INCLUDE_PATH) -I$(LIBCACA_INCLUDE_PATH) $(LCRCFLAGS)
CC=$(LCRCC)
OUTNAME=gdalcacaview

all: gdalcacaview

.c.o:
	$(CC) $(CFLAGS) -c $<

gdalcacaview: gdalcacaview.o Makefile
	$(CC) gdalcacaview.o $(GDAL) $(LCRLDFLAGS) -o $(OUTNAME)

install:
	cp gdalcacaview $(LIBCACA_BIN_PATH)

clean:
	rm -f *.o $(OUTNAME)
	
