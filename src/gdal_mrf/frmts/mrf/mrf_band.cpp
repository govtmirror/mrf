
/*
* Copyright (c) 2002-2012, California Institute of Technology.
* All rights reserved.  Based on Government Sponsored Research under contracts NAS7-1407 and/or NAS7-03001.

* Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following conditions are met:
*   1. Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.
*   2. Redistributions in binary form must reproduce the above copyright notice, 
*      this list of conditions and the following disclaimer in the documentation and/or other materials provided with the distribution.
*   3. Neither the name of the California Institute of Technology (Caltech), its operating division the Jet Propulsion Laboratory (JPL), 
*      the National Aeronautics and Space Administration (NASA), nor the names of its contributors may be used to 
*      endorse or promote products derived from this software without specific prior written permission.

* THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, 
* INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. 
* IN NO EVENT SHALL THE CALIFORNIA INSTITUTE OF TECHNOLOGY BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, 
* EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; 
* LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, 
* STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, 
* EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

*/


/******************************************************************************
* $Id$
*
* Project:  Meta Raster File Format Driver Implementation, RasterBand
* Purpose:  Implementation of Pile of Tile Format
*
* Author:   Lucian Plesea, Lucian.Plesea@jpl.nasa.gov, lplesea@esri.com
*
******************************************************************************
*
*
* 
*
* 
****************************************************************************/

#include "marfa.h"
#include <gdal_priv.h>
#include <ogr_srs_api.h>
#include <ogr_spatialref.h>

#include <vector>
#include <assert.h>
#include "../zlib/zlib.h"

using std::vector;
using std::string;

// packs a block of a given type, with a stride
// Count is the number of items that need to be copied
// These are separate to allow for optimization

template <typename T> void cpy_stride_in(void *dst, 
        const void *src, int c, int stride)
{
    T *s=(T *)src;
    T *d=(T *)dst;

    while (c--) {
        *d++=*s;
        s+=stride;
    }
}

template <typename T> void cpy_stride_out(void *dst, 
        const void *src, int c, int stride)
{
    T *s=(T *)src;
    T *d=(T *)dst;

    while (c--) {
        *d=*s++;
        d+=stride;
    }
}

// Is the buffer filled with zeros
inline int is_zero(const char *b,size_t count)
{
    while (count--) if (*b++) return 0;
    return TRUE;
}

// Does every byte in the buffer have the same value
inline int is_empty(const char *b,size_t count, char val=0)

{
    while (count--) if (*(b++)!=val) return 0;
    return TRUE;
}

// Swap bytes in place, unconditional
static void swab_buff(buf_mgr &src, const ILImage &img)
{
    switch (GDALGetDataTypeSize(img.dt)) {
    case 16: {
	short int *b=(short int*)src.buffer;
	for (int i=src.size/2;i;b++,i--) 
	    *b=swab16(*b);
	break;
	     }
    case 32: {
	int *b=(int*)src.buffer;
	for (int i=src.size/4;i;b++,i--) 
	    *b=swab32(*b);
	break;
	     }
    case 64: {
	long long *b=(long long*)src.buffer;
	for (int i=src.size/8;i;b++,i--)
	    *b=swab64(*b);
	break;
	     }
    }
}

/**
*\brief Deflates a buffer, extrasize is the available size in the buffer past the input
*  If the output fits past the data, it uses that area
* otherwise it uses a temporary buffer and copies the data over the input on return, returning a pointer to it
*/
static void *DeflateBlock(buf_mgr &src, size_t extrasize, int flags) {
    // The one we might need to allocate
    void *dbuff = NULL;
    buf_mgr dst;
    // The one we could use, after the packed data
    dst.buffer = src.buffer + src.size;
    dst.size = extrasize;

    // Allocate a temp buffer if there is not sufficient space,
    // We need to have a bit more than half the buffer available
    if (extrasize < (src.size + 64)) {
	dst.size = src.size + 64;

	dbuff = CPLMalloc(dst.size);
	dst.buffer = (char *)dbuff;
	if (!dst.buffer)
	    return NULL;
    }

    if (!ZPack(src, dst, flags)) {
	CPLFree(dbuff); // Safe to call with NULL
	return NULL;
    }

    // source size is used to hold the output size
    src.size = dst.size;
    // If we didnt' allocate a buffer, the receiver can use it already
    if (!dbuff) 
	return dst.buffer;

    // If we allocated a buffer, we need to copy the data to the input buffer 
    memcpy(src.buffer, dbuff, src.size);
    CPLFree(dbuff);
    return src.buffer;
}

GDALMRFRasterBand::GDALMRFRasterBand(GDALMRFDataset *parent_dataset,
					const ILImage &image, int band, int ov)
{
    poDS=parent_dataset;
    nBand=band;
    m_band=band-1;
    m_l=ov;
    img=image;
    eDataType=parent_dataset->current.dt;
    nRasterXSize = img.size.x;
    nRasterYSize = img.size.y;
    nBlockXSize = img.pagesize.x;
    nBlockYSize = img.pagesize.y;
    nBlocksPerRow = img.pcount.x;
    nBlocksPerColumn = img.pcount.y;
    img.NoDataValue = GetNoDataValue(&img.hasNoData);
    deflate = CSLFetchBoolean(poDS->optlist, "DEFLATE", FALSE);
    // Bring the quality to 0 to 9
    deflate_flags = img.quality / 10;
    // Pick up the twists, aka GZ, RAWZ headers
    if (CSLFetchBoolean(poDS->optlist, "GZ", FALSE))
	deflate_flags |= ZFLAG_GZ;
    else if (CSLFetchBoolean(poDS->optlist, "RAWZ", FALSE))
	deflate_flags |= ZFLAG_RAW;
    // And Pick up the ZLIB strategy, if any
    const char *zstrategy = CSLFetchNameValueDef(poDS->optlist, "Z_STRATEGY", NULL);
    if (zstrategy) {
	int zv = 0;
	if (EQUAL(zstrategy, "Z_HUFFMAN_ONLY"))
	    zv = Z_HUFFMAN_ONLY;
	else if (EQUAL(zstrategy, "Z_RLE"))
	    zv = Z_RLE;
	else if (EQUAL(zstrategy, "Z_FILTERED"))
	    zv = Z_FILTERED;
	else if (EQUAL(zstrategy, "Z_FIXED"))
	    zv = Z_FIXED;
	deflate_flags |= (zv << 6);
    }
}

// Clean up the overviews if they exist
GDALMRFRasterBand::~GDALMRFRasterBand()
{
    while (0!=overviews.size()) {
	delete overviews[overviews.size()-1];
	overviews.pop_back();
    };
}

// Look for a string from the dataset options or from the environment
const char * GDALMRFRasterBand::GetOptionValue(const char *opt, const char *def)
{
    const char *optValue = CSLFetchNameValue(poDS->optlist, opt);
    if (0 != optValue)
	return optValue;
    return CPLGetConfigOption(opt, def);
}

// Utility function, returns a value from a vector corresponding to the band index
// or the first entry
static double getBandValue(std::vector<double> &v,int idx)
{
    if (static_cast<int>(v.size()) > idx)
	return v[idx];
    return v[0];
}

double GDALMRFRasterBand::GetNoDataValue(int *pbSuccess)
{
    std::vector<double> &v=poDS->vNoData;
    if (v.size() == 0)
	return GDALPamRasterBand::GetNoDataValue(pbSuccess);
    if (pbSuccess) *pbSuccess=TRUE;
    return getBandValue(v, m_band);
}

double GDALMRFRasterBand::GetMinimum(int *pbSuccess)
{
    std::vector<double> &v=poDS->vMin;
    if (v.size() == 0)
	return GDALPamRasterBand::GetMinimum(pbSuccess);
    if (pbSuccess) *pbSuccess=TRUE;
    return getBandValue(v, m_band);
}

double GDALMRFRasterBand::GetMaximum(int *pbSuccess)
{
    std::vector<double> &v=poDS->vMax;
    if (v.size() == 0)
	return GDALPamRasterBand::GetMaximum(pbSuccess);
    if (pbSuccess) *pbSuccess=TRUE;
    return getBandValue(v, m_band);
}

// Fill, with ndv
template<typename T> CPLErr buff_fill(void *b, size_t count, const T ndv)
{
    T *buffer = static_cast<T*>(b);
    count /= sizeof(T);
    while (count--)
	*buffer++ = ndv;
    return CE_None;
}

/**
*\brief Fills a buffer with no data
*
*/
CPLErr GDALMRFRasterBand::FillBlock(void *buffer)
{
    double ndv = img.NoDataValue;
    size_t bsb = blockSizeBytes();

    // use 0 if NoData is not defined
    if (!img.hasNoData) ndv = 0.0L;
    // use memset for speed for bytes, or if nodata is not defined
    if (!img.hasNoData || eDataType == GDT_Byte) {
	memset(buffer, ndv, bsb);
	return CE_None;
    }

#define bf(T) buff_fill<T>(buffer, bsb, T(ndv));
    switch(eDataType) {
	case GDT_UInt16:    return bf(GUInt16);
	case GDT_Int16:     return bf(GInt16);
	case GDT_UInt32:    return bf(GUInt32);
	case GDT_Int32:     return bf(GInt32);
	case GDT_Float32:   return bf(float);
	case GDT_Float64:   return bf(double);
    }
#undef bf

    return CE_Failure;
}

/*\brief Interleave block read
 *
 *  Acquire space for all the other bands, unpack there, then drop the locks
 *  The current band output goes directly into the buffer
 */

CPLErr GDALMRFRasterBand::RB(int xblk, int yblk, buf_mgr src, void *buffer) {
    vector<GDALRasterBlock *> blocks;

    for (int i = 0; i < poDS->nBands; i++) {
	GDALRasterBand *b = poDS->GetRasterBand(i+1);
	if (b->GetOverviewCount() && m_l)
	    b = b->GetOverview(m_l-1);

	void *ob = buffer;
	// Get the other band blocks, keep them around until later
	if (b != this)
	{
	    GDALRasterBlock *poBlock = b->GetLockedBlockRef(xblk, yblk, 1);
	    ob = poBlock->GetDataRef();
	    blocks.push_back(poBlock);
	} 

	// Just the right mix of templates and macros make deinterleaving tidy
#define CpySI(T) cpy_stride_in<T> (ob, (T *)poDS->pbuffer + i,\
    blockSizeBytes()/sizeof(T), img.pagesize.c)

	// Page is already in poDS->pbuffer, not empty
	switch (GDALGetDataTypeSize(eDataType)/8)
	{
	case 1: CpySI(GByte); break;
	case 2: CpySI(GInt16); break;
	case 4: CpySI(GInt32); break;
	case 8: CpySI(GIntBig); break;
	}
    }

#undef CpySI

    // Drop the locks we acquired
    for (int i=0; i<blocks.size(); i++)
	blocks[i]->DropLock();

    return CE_None;
}

/**
*\brief Fetch a block from the backing store dataset and keep a copy in the cache
*
* @xblk The X block number, zero based
* @yblk The Y block number, zero based
* @param tinfo The return, updated tinfo for this specific tile
*
*/
CPLErr GDALMRFRasterBand::FetchBlock(int xblk, int yblk, void *buffer)

{
    CPLDebug("MRF_IB","FetchBlock %d,%d,0,%d, level  %d\n", xblk, yblk, m_band, m_l);

    // Paranoid checks, should never happen
    if (poDS->source.empty()) {
	CPLError( CE_Failure, CPLE_AppDefined, "MRF: No cached source image to fetch from");
	return CE_Failure;
    }

    if (poDS->clonedSource)  // This is a clone
	return FetchClonedBlock(xblk, yblk, buffer);

    GDALDataset *poSrcDS;
    GInt32 cstride = img.pagesize.c;
    ILSize req(xblk, yblk, 0, m_band/cstride, m_l);
    GUIntBig infooffset = IdxOffset(req, img);

    if ( 0 == (poSrcDS = poDS->GetSrcDS())) {
	CPLError( CE_Failure, CPLE_AppDefined, "MRF: Can't open source file %s", poDS->source.c_str());
	return CE_Failure;
    }

    // Scale to base resolution
    double scl = pow(poDS->scale, m_l);
    if ( 0 == m_l )
	scl = 1; // To allow for precision issues

    // Prepare parameters for RasterIO, they might be different from a full page
    int vsz = GDALGetDataTypeSize(eDataType)/8;
    int Xoff = int(xblk * img.pagesize.x * scl + 0.5);
    int Yoff = int(yblk * img.pagesize.y * scl + 0.5);
    int readszx = int(img.pagesize.x * scl + 0.5);
    int readszy = int(img.pagesize.y * scl + 0.5);

    // Compare with the full size and clip to the right and bottom if needed
    int clip=0;
    if (Xoff + readszx > poDS->full.size.x) {
	clip |= 1;
	readszx = poDS->full.size.x - Xoff;
    }
    if (Yoff + readszy > poDS->full.size.y) {
	clip |= 1;
	readszy = poDS->full.size.y - Yoff;
    }

    // This is where the whole page fits
    void *ob = buffer;
    if (cstride != 1) 
	ob = poDS->pbuffer;

    // Fill buffer with NoData if clipping
    if (clip) 
	FillBlock(ob);

    // Use the dataset RasterIO if reading all bands
    CPLErr ret = poSrcDS->RasterIO( GF_Read, Xoff, Yoff, readszx, readszy,
	ob, pcount(readszx, int(scl)), pcount(readszy, int(scl)),
	eDataType, cstride, (cstride==1)? &nBand:NULL,
	// pixel, line, band stride
	vsz * img.pagesize.c,
	vsz * img.pagesize.c * img.pagesize.x, 
	vsz * img.pagesize.c * img.pagesize.x * img.pagesize.y );

    if (ret != CE_None)
	return ret;
    // Might have the block in the pbuffer, mark it
    poDS->tile = req;

    // If it should not be stored, mark it as such
    if (eDataType == GDT_Byte && poDS->vNoData.size()) {
	if (is_empty((char *)ob, img.pageSizeBytes, (char)GetNoDataValue(0)))
	    return poDS->WriteTile((void *)1, infooffset, 0);
    } else if (is_zero((char *)ob, img.pageSizeBytes))
	return poDS->WriteTile((void *)1, infooffset, 0);

    // Write the page in the local cache
    buf_mgr filesrc={(char *)ob, img.pageSizeBytes};

    // Have to use a separate buffer for compression output.
    void *outbuff = CPLMalloc(poDS->pbsize);

    if (!outbuff) {
	CPLError(CE_Failure, CPLE_AppDefined, 
	    "Can't get buffer for writing page");
	// This is not really an error for a cache, the data is fine
	return CE_Failure;
    }

    buf_mgr filedst={(char *)outbuff, poDS->pbsize};
    Compress(filedst, filesrc);

    // Where the output is, in case we deflate
    void *usebuff = outbuff;
    if (deflate) {
	usebuff = DeflateBlock( filedst, poDS->pbsize - filedst.size, deflate_flags);
	if (!usebuff) {
	    CPLError(CE_Failure,CPLE_AppDefined, "MRF: Deflate error");
	    return CE_Failure;
	}
    }

    // Write and update the tile index
    ret = poDS->WriteTile(usebuff, infooffset, filedst.size);
    CPLFree(outbuff);

    // If we hit an error or if unpaking is not needed
    if (ret != CE_None || cstride == 1)
	return ret;

    // data is already in filesrc buffer, deinterlace it in pixel blocks
    return RB(xblk, yblk, filesrc, buffer);
}


/**
*\brief Fetch for a cloned MRF
*
* @xblk The X block number, zero based
* @yblk The Y block number, zero based
* @param tinfo The return, updated tinfo for this specific tile
*
*/

CPLErr GDALMRFRasterBand::FetchClonedBlock(int xblk, int yblk, void *buffer)
{
    CPLDebug("MRF_IB","FetchClonedBlock %d,%d,0,%d, level  %d\n", xblk, yblk, m_band, m_l);

    VSILFILE *srcfd;
    // Paranoid check
    assert(poDS->clonedSource);

    GDALMRFDataset *poSrc;
    if ( 0 == (poSrc = static_cast<GDALMRFDataset *>(poDS->GetSrcDS()))) {
	CPLError( CE_Failure, CPLE_AppDefined, "MRF: Can't open source file %s", poDS->source.c_str());
	return CE_Failure;
    }

    if (DataMode() == GF_Read) {
	// Can't store, so just fetch from source, which is an MRF with the same structure
	GDALMRFRasterBand *b = static_cast<GDALMRFRasterBand *>(poSrc->GetRasterBand(nBand));
	if (b->GetOverviewCount() && m_l)
	    b = static_cast<GDALMRFRasterBand *>(b->GetOverview(m_l-1));
	return b->IReadBlock(xblk,yblk,buffer);
    }

    ILSize req(xblk, yblk, 0, m_band/img.pagesize.c , m_l);
    ILIdx tinfo;

    // Get the cloned source tile info
    // The cloned source index is after the current one
    if (CE_None != poDS->ReadTileIdx(tinfo, req, img, poDS->idxSize)) {
	CPLError(CE_Failure, CPLE_AppDefined, "MRF: Unable to read cloned index entry");
	return CE_Failure;
    }

    GUIntBig infooffset = IdxOffset(req, img);
    CPLErr err;

    // Does the source have this tile?
    if (tinfo.size == 0) { // Nope, mark it empty and return fill
	err = poDS->WriteTile((void *)1, infooffset, 0);
	if (CE_None != err)
	    return err;
	return FillBlock(buffer);
    }

    srcfd = poSrc->DataFP();
    if (NULL == srcfd) {
	CPLError( CE_Failure, CPLE_AppDefined, "MRF: Can't open source data file %s", 
	    poDS->source.c_str());
	return CE_Failure;
    }

    // Need to read the tile from the source
    char *buf = static_cast<char *>(CPLMalloc(tinfo.size));

    VSIFSeekL(srcfd, tinfo.offset, SEEK_SET);
    if (tinfo.size != VSIFReadL( buf, 1, tinfo.size, srcfd) ) {
	CPLFree(buf);
	CPLError( CE_Failure, CPLE_AppDefined, "MRF: Can't read data from source %s",
	    poSrc->current.datfname.c_str() );
	return CE_Failure;
    }

    // Write it then reissue the read
    err = poDS->WriteTile(buf, infooffset, tinfo.size);
    CPLFree(buf);
    if ( CE_None != err )
	return err;
    // Reissue read, it will work from the cloned data
    return IReadBlock(xblk, yblk, buffer);
}


/**
*\brief read a block in the provided buffer
* 
*  For separate band model, the DS buffer is not used, the read is direct in the buffer
*  For pixel interleaved model, the DS buffer holds the temp copy
*  and all the other bands are force read
*
*/

CPLErr GDALMRFRasterBand::IReadBlock(int xblk, int yblk, void *buffer)
{
    ILIdx tinfo;
    GInt32 cstride=img.pagesize.c;
    ILSize req(xblk,yblk,0,m_band/cstride,m_l);
    CPLDebug("MRF_IB", "IReadBlock %d,%d,0,%d, level %d\n", xblk, yblk, m_band, m_l);

    if (CE_None != poDS->ReadTileIdx(tinfo, req, img)) {
	CPLError( CE_Failure, CPLE_AppDefined,
	    "MRF: Unable to read index at offset %lld", IdxOffset(req, img));
	return CE_Failure;
    }

    if (0 == tinfo.size) { // Could be missing or it could be caching
	// Offset != 0 means no data, Update mode is for local MRFs only
	// if caching index mode is RO don't try to fetch
	// Also, caching MRFs can't be opened in update mode
	if ( 0 != tinfo.offset || GA_Update == poDS->eAccess 
	    || poDS->source.empty() || IdxMode() == GF_Read )
	    return FillBlock(buffer);

	// caching MRF, need to fetch a block
	return FetchBlock(xblk, yblk, buffer);
    }

    CPLDebug("MRF_IB","Tinfo offset %lld, size %lld\n", tinfo.offset, tinfo.size);
    // If we have a tile, read it

    // Should use a permanent buffer, like the pbuffer mechanism
    // Get a large buffer, in case we need to unzip
    void *data = CPLMalloc(tinfo.size);

    VSILFILE *dfp = DataFP();

    // No data file to read from
    if (dfp == NULL)
	return CE_Failure;

    // This part is not thread safe, but it is what GDAL expects
    VSIFSeekL(dfp, tinfo.offset, SEEK_SET);
    if (1 != VSIFReadL(data, tinfo.size, 1, dfp)) {
	CPLFree(data);
	CPLError(CE_Failure, CPLE_AppDefined, "Unable to read data page, %ld@%lx",
	    tinfo.size, tinfo.offset);
	return CE_Failure;
    }

    buf_mgr src = {(char *)data, tinfo.size};
    buf_mgr dst;

    // We got the data, do we need to decompress it before decoding?
    if (deflate) {
	dst.size = img.pageSizeBytes + 1440; // in case the packed page is a bit larger than the raw one
	dst.buffer = (char *)CPLMalloc(dst.size);

	if (ZUnPack(src, dst, deflate_flags)) {
	    // Got it unpacked, update the pointers
	    CPLFree(data);
	    tinfo.size = dst.size;
	    data = dst.buffer;
	} else { // Warn and assume the data was not deflated
	    CPLError(CE_Warning, CPLE_AppDefined, "Can't inflate page!");
	    CPLFree(dst.buffer);
	}
    }

    src.buffer = (char *)data;
    src.size = tinfo.size;

    // After unpacking, the size has to be pageSizeBytes
    dst.buffer = (char *)buffer;
    dst.size = img.pageSizeBytes;

    // If pages are interleaved, use the dataset page buffer instead
    if (1!=cstride)
	dst.buffer = (char *)poDS->pbuffer;

    CPLErr ret = Decompress(dst, src);
    dst.size = img.pageSizeBytes; // In case the decompress failed, force it back
    CPLFree(data);

    // Swap whatever we decompressed if we need to
    if (is_Endianess_Dependent(img.dt,img.comp) && (img.nbo != NET_ORDER) ) 
	swab_buff(dst, img);

    // If pages are separate, we're done, the read was in the output buffer
    if ( 1 == cstride || CE_None != ret)
	return ret;

    // De-interleave page and return
    return RB(xblk, yblk, dst, buffer);
}


/**
*\brief Write a block from the provided buffer
* 
* Same trick as read, use a temporary tile buffer for pixel interleave
* For band separate, use a 
* Write the block once it has all the bands, report 
* if a new block is started before the old one was completed
*
*/

CPLErr GDALMRFRasterBand::IWriteBlock(int xblk, int yblk, void *buffer)

{
    GInt32 cstride = img.pagesize.c;
    ILSize req(xblk, yblk, 0, m_band/cstride, m_l);
    GUIntBig infooffset = IdxOffset(req, img);

    CPLDebug("MRF_IB", "IWriteBlock %d,%d,0,%d, level  %d, stride %d\n", xblk, yblk, 
	m_band, m_l, cstride);

    if (1 == cstride) {     // Separate bands, we can write it as is
	// Empty page skip. Byte data only, the NoData needs more work
	if ((eDataType==GDT_Byte) && (poDS->vNoData.size())) {
	    if (is_empty((char *)buffer, img.pageSizeBytes, char(GetNoDataValue(0))))
		return poDS->WriteTile(0, infooffset, 0);
	} else if (is_zero((char *)buffer, img.pageSizeBytes)) // Don't write buffers with zero
	    return poDS->WriteTile(0, infooffset, 0);

	// Use the pbuffer to hold the compressed page before writing it
	poDS->tile = ILSize(); // Mark it corrupt

	buf_mgr src = {(char *)buffer, img.pageSizeBytes};
	buf_mgr dst = {(char *)poDS->pbuffer, poDS->pbsize};

	// Swab the source before encoding if we need to 
	if (is_Endianess_Dependent(img.dt, img.comp) && (img.nbo != NET_ORDER)) 
	    swab_buff(src, img);

	// Compress functions need to return the compresed size in
	// the bytes in buffer field
	Compress(dst, src);
	void *usebuff = dst.buffer;
	if (deflate) {
	    usebuff = DeflateBlock(dst, poDS->pbsize - dst.size, deflate_flags);
	    if (!usebuff) {
		CPLError(CE_Failure,CPLE_AppDefined, "MRF: Deflate error");
		return CE_Failure;
	    }
	}
	return poDS->WriteTile(usebuff, infooffset , dst.size);
    }

    // Multiple bands per page, use a temporary to assemble the page
    // Temporary is large because we use it to hold both the uncompressed and the compressed
    poDS->tile=req; poDS->bdirty=0;

    // Keep track of what bands are empty
    GUIntBig empties=0;

    void *tbuffer = CPLMalloc(img.pageSizeBytes + poDS->pbsize);

    if (!tbuffer) {
    	CPLError(CE_Failure,CPLE_AppDefined, "MRF: Can't allocate write buffer");
	return CE_Failure;
    }
	
    // Get the other bands from the block cache
    for (int iBand=0; iBand < poDS->nBands; iBand++ )
    {
	const char *pabyThisImage=NULL;
	GDALRasterBlock *poBlock=NULL;

	if (iBand == m_band)
	{
	    pabyThisImage = (char *) buffer;
	    poDS->bdirty |= bandbit();
	} else {
	    GDALRasterBand *band = poDS->GetRasterBand(iBand +1);
	    // Pick the right overview
	    if (m_l) band = band->GetOverview(m_l -1);
	    poBlock = ((GDALMRFRasterBand *)band)
		->TryGetLockedBlockRef(xblk, yblk);
	    if (NULL==poBlock) continue;
	    // This is where the image data is for this band

	    pabyThisImage = (char*) poBlock->GetDataRef();
	    poDS->bdirty |= bandbit(iBand);
	}

	// Keep track of empty bands, but encode them anyhow, in case some are not empty
	if ((eDataType == GDT_Byte) && poDS->vNoData.size()) {
	    if (is_empty(pabyThisImage, blockSizeBytes(), (char)GetNoDataValue(0)))
		empties |= bandbit(iBand);
        } else if (is_zero(pabyThisImage, blockSizeBytes()))
	    empties |= bandbit(iBand);

	// Copy the data into the dataset buffer here
	// Just the right mix of templates and macros make this real tidy
#define CpySO(T) cpy_stride_out<T> (((T *)tbuffer)+iBand, pabyThisImage,\
		blockSizeBytes()/sizeof(T), cstride)

	// Build the page in tbuffer
	switch (GDALGetDataTypeSize(eDataType)/8)
	{
	    case 1: CpySO(GByte); break;
	    case 2: CpySO(GInt16); break;
	    case 4: CpySO(GInt32); break;
	    case 8: CpySO(GIntBig); break;
	    default:
		CPLError(CE_Failure,CPLE_AppDefined, "MRF: Write datatype of %d bytes "
			"not implemented", GDALGetDataTypeSize(eDataType)/8);
		return CE_Failure;
	}

	if (poBlock != NULL)
	{
	    poBlock->MarkClean();
	    poBlock->DropLock();
	}
    }

    if (empties == AllBandMask()) {
	CPLFree(tbuffer);
	return poDS->WriteTile(0, infooffset, 0);
    }

    if (poDS->bdirty != AllBandMask())
	CPLError(CE_Warning, CPLE_AppDefined,
	"MRF: IWrite, band dirty mask is %0llx instead of %lld",
	poDS->bdirty, AllBandMask());

//    ppmWrite("test.ppm",(char *)tbuffer, ILSize(nBlockXSize,nBlockYSize,0,poDS->nBands));

    buf_mgr src={(char *)tbuffer, img.pageSizeBytes};

    // Use the space after pagesizebytes for compressed output, it is of pbsize
    char *outbuff = (char *)tbuffer + img.pageSizeBytes;

    buf_mgr dst = {outbuff, poDS->pbsize};
    Compress(dst, src);

    // Where the output is, in case we deflate
    void *usebuff = outbuff;
    if (deflate) {
	// Move the packed part at the start of tbuffer, to make more space available
	memcpy(tbuffer, outbuff, dst.size);
	dst.buffer = (char *)tbuffer;
	usebuff = DeflateBlock(dst, img.pageSizeBytes + poDS->pbsize - dst.size, deflate_flags);
	if (!usebuff) {
	    CPLError(CE_Failure,CPLE_AppDefined, "MRF: Deflate error");
	    CPLFree(tbuffer);
	    return CE_Failure;
	}
    }

    CPLErr ret = poDS->WriteTile(usebuff, infooffset, dst.size);
    CPLFree(tbuffer);

    poDS->bdirty = 0;
    return ret;
}
