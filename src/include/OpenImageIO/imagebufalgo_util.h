/*
  Copyright 2013 Larry Gritz and the other authors and contributors.
  All Rights Reserved.

  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the following conditions are
  met:
  * Redistributions of source code must retain the above copyright
    notice, this list of conditions and the following disclaimer.
  * Redistributions in binary form must reproduce the above copyright
    notice, this list of conditions and the following disclaimer in the
    documentation and/or other materials provided with the distribution.
  * Neither the name of the software's owners nor the names of its
    contributors may be used to endorse or promote products derived from
    this software without specific prior written permission.
  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
  A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
  OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
  SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
  LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
  DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
  THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
  (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
  OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

  (This is the Modified BSD License)
*/


#ifndef OPENIMAGEIO_IMAGEBUFALGO_UTIL_H
#define OPENIMAGEIO_IMAGEBUFALGO_UTIL_H

#include <OpenImageIO/platform.h>
#include <OpenImageIO/imagebufalgo.h>
#include <OpenImageIO/thread.h>

#if OIIO_CPLUSPLUS_VERSION >= 11
# include <functional>
#else
# include <boost/bind.hpp>
#endif


OIIO_NAMESPACE_BEGIN

#if OIIO_CPLUSPLUS_VERSION >= 11
using std::bind;
using std::ref;
using std::cref;
using namespace std::placeholders;
using std::placeholders::_1;
#else
using boost::bind;
using boost::ref;
using boost::cref;
using boost::this_thread::get_id;
#endif



namespace ImageBufAlgo {

enum SplitDir { Split_X, Split_Y, Split_Z, Split_Biggest };

/// Helper template for generalized multithreading for image processing
/// functions.  Some function/functor f is applied to every pixel the
/// region of interest roi, dividing the region into multiple threads if
/// threads != 1.  Note that threads == 0 indicates that the number of
/// threads should be as set by the global OIIO "threads" attribute.
///
/// The optional splitdir determines along which axis the split will be
/// made. The default is Split_Y (vertical splits), which generally seems
/// the fastest (due to cache layout issues?), but perhaps there are
/// algorithms where it's better to split in X, Z, or along the longest
/// axis.
///
/// Most image operations will require additional arguments, including
/// additional input and output images or other parameters.  The
/// parallel_image template can still be used by employing the
/// std::bind. For example, suppose you have an image operation defined as:
///     void my_image_op (ImageBuf &out, const ImageBuf &in,
///                       float scale, ROI roi);
/// Then you can parallelize it as follows:
///     ImageBuf R, A;   // result, input
///     ROI roi = get_roi (R.spec());
///     parallel_image (bind(my_image_op,ref(R), cref(A),3.14,_1), roi);
///
template <class Func>
void
parallel_image (Func f, ROI roi, int nthreads=0, SplitDir splitdir=Split_Y)
{
    // Special case: threads <= 0 means to use the "threads" attribute
    if (nthreads <= 0)
        OIIO::getattribute ("threads", nthreads);
    // Try not to assign a thread less than 16k pixels, or it's not worth
    // the thread startup/teardown cost.
    nthreads = std::min (nthreads, 1 + int(roi.npixels() / size_t(16384)));
    if (nthreads <= 1) {
        // Just one thread, or a small image region: use this thread only
        f (roi);
        return;
    }

    // If splitdir was not explicit, find the longest edge.
    if (splitdir >= Split_Biggest)
        splitdir = roi.width() > roi.height() ? Split_X : Split_Y;
    int minmax[6] = { roi.xbegin, roi.xend, roi.ybegin, roi.yend,
                      roi.zbegin, roi.zend };
    int roi_begin = minmax[2*int(splitdir)];
    int roi_end = minmax[2*int(splitdir)+1];
    int splitlen = roi_end - roi_begin;
    nthreads = std::min (nthreads, splitlen);

    // Spawn threads by dividing the region into bands.
    OIIO::thread_group threads;
    int blocksize = std::max (1, (splitlen + nthreads - 1) / nthreads);
    for (int i = 0;  i < nthreads;  i++) {
        if (splitdir == Split_Y) {
            roi.ybegin = roi_begin + i * blocksize;
            roi.yend = std::min (roi.ybegin + blocksize, roi_end);
            if (roi.ybegin >= roi.yend)
                break;   // no more work to dole out
        } else if (splitdir == Split_X) {
            roi.xbegin = roi_begin + i * blocksize;
            roi.xend = std::min (roi.xbegin + blocksize, roi_end);
            if (roi.xbegin >= roi.xend)
                break;   // no more work to dole out
        } else { // if (splitdir == Split_Z)
            roi.zbegin = roi_begin + i * blocksize;
            roi.zend = std::min (roi.zbegin + blocksize, roi_end);
            if (roi.zbegin >= roi.zend)
                break;   // no more work to dole out
        }
        if (i < nthreads-1)
            threads.add_thread (new OIIO::thread (f, roi));
        else
            f (roi);   // Run the last one in the calling thread
    }
    threads.join_all ();
}



/// Common preparation for IBA functions: Given an ROI (which may or may not
/// be the default ROI::All()), destination image (which may or may not yet
/// be allocated), and optional input images, adjust roi if necessary and
/// allocate pixels for dst if necessary.  If dst is already initialized, it
/// will keep its "full" (aka display) window, otherwise its full/display
/// window will be set to the union of A's and B's full/display windows.  If
/// dst is uninitialized and  force_spec is not NULL, use *force_spec as
/// dst's new spec rather than using A's.  Also, if A or B inputs are
/// specified but not initialized or broken, it's an error so return false.
/// If all is ok, return true.  Some additional checks and behaviors may be
/// specified by the 'prepflags', which is a bit field defined by
/// IBAprep_flags.
bool OIIO_API IBAprep (ROI &roi, ImageBuf *dst, const ImageBuf *A=NULL,
                       const ImageBuf *B=NULL, const ImageBuf *C=NULL,
                       ImageSpec *force_spec=NULL, int prepflags=0);
inline bool IBAprep (ROI &roi, ImageBuf *dst, const ImageBuf *A,
                     const ImageBuf *B, ImageSpec *force_spec,
                     int prepflags=0) {
    return IBAprep (roi, dst, A, B, NULL, force_spec, prepflags);
}
inline bool IBAprep (ROI &roi, ImageBuf *dst,
                     const ImageBuf *A, int prepflags) {
    return IBAprep (roi, dst, A, NULL, NULL, NULL, prepflags);
}

enum IBAprep_flags {
    IBAprep_DEFAULT = 0,
    IBAprep_REQUIRE_ALPHA = 1<<0,
    IBAprep_REQUIRE_Z = 1<<1,
    IBAprep_REQUIRE_SAME_NCHANNELS = 1<<2,
    IBAprep_NO_COPY_ROI_FULL = 1<<3,    // Don't copy the src's roi_full
    IBAprep_NO_SUPPORT_VOLUME = 1<<4,   // Don't know how to do volumes
    IBAprep_NO_COPY_METADATA = 1<<8,    // N.B. default copies all metadata
    IBAprep_COPY_ALL_METADATA = 1<<9,   // Even unsafe things
    IBAprep_CLAMP_MUTUAL_NCHANNELS = 1<<10, // Clamp roi.chend to max of inputs
    IBAprep_SUPPORT_DEEP = 1<<11,       // Operation allows deep images
    IBAprep_DEEP_MIXED = 1<<12,         // Allow deep & non-deep combinations
    IBAprep_DST_FLOAT_PIXELS = 1<<13,   // If dst is uninit, make it float
    IBAprep_MINIMIZE_NCHANNELS = 1<<14, // Multi-inputs get min(nchannels)
};



/// Given data types a and b, return a type that is a best guess for one
/// that can handle both without any loss of range or precision.
TypeDesc::BASETYPE OIIO_API type_merge (TypeDesc::BASETYPE a, TypeDesc::BASETYPE b);

inline TypeDesc::BASETYPE
type_merge (TypeDesc::BASETYPE a, TypeDesc::BASETYPE b, TypeDesc::BASETYPE c)
{
    return type_merge (type_merge(a,b), c);
}

inline TypeDesc type_merge (TypeDesc a, TypeDesc b) {
    return type_merge (TypeDesc::BASETYPE(a.basetype), TypeDesc::BASETYPE(b.basetype));
}

inline TypeDesc type_merge (TypeDesc a, TypeDesc b, TypeDesc c)
{
    return type_merge (type_merge(a,b), c);
}



// Macro to call a type-specialzed version func<type>(R,...)
#define OIIO_DISPATCH_TYPES(ret,name,func,type,R,...)                   \
    switch (type.basetype) {                                            \
    case TypeDesc::FLOAT :                                              \
        ret = func<float> (R, __VA_ARGS__); break;                      \
    case TypeDesc::UINT8 :                                              \
        ret = func<unsigned char> (R, __VA_ARGS__); break;              \
    case TypeDesc::HALF  :                                              \
        ret = func<half> (R, __VA_ARGS__); break;                       \
    case TypeDesc::UINT16:                                              \
        ret = func<unsigned short> (R, __VA_ARGS__); break;             \
    case TypeDesc::INT8  :                                              \
        ret = func<char> (R, __VA_ARGS__); break;                       \
    case TypeDesc::INT16 :                                              \
        ret = func<short> (R, __VA_ARGS__); break;                      \
    case TypeDesc::UINT  :                                              \
        ret = func<unsigned int> (R, __VA_ARGS__); break;               \
    case TypeDesc::INT   :                                              \
        ret = func<int> (R, __VA_ARGS__); break;                        \
    case TypeDesc::DOUBLE:                                              \
        ret = func<double> (R, __VA_ARGS__); break;                     \
    default:                                                            \
        (R).error ("%s: Unsupported pixel data format '%s'", name, type); \
        ret = false;                                                    \
    }

// Helper, do not call from the outside world.
#define OIIO_DISPATCH_TYPES2_HELP(ret,name,func,Rtype,Atype,R,...)      \
    switch (Atype.basetype) {                                           \
    case TypeDesc::FLOAT :                                              \
        ret = func<Rtype,float> (R, __VA_ARGS__); break;                \
    case TypeDesc::UINT8 :                                              \
        ret = func<Rtype,unsigned char> (R, __VA_ARGS__); break;        \
    case TypeDesc::HALF  :                                              \
        ret = func<Rtype,half> (R, __VA_ARGS__); break;                 \
    case TypeDesc::UINT16:                                              \
        ret = func<Rtype,unsigned short> (R, __VA_ARGS__); break;       \
    case TypeDesc::INT8 :                                               \
        ret = func<Rtype,char> (R, __VA_ARGS__); break;                 \
    case TypeDesc::INT16 :                                              \
        ret = func<Rtype,short> (R, __VA_ARGS__); break;                \
    case TypeDesc::UINT :                                               \
        ret = func<Rtype,unsigned int> (R, __VA_ARGS__); break;         \
    case TypeDesc::INT :                                                \
        ret = func<Rtype,int> (R, __VA_ARGS__); break;                  \
    case TypeDesc::DOUBLE :                                             \
        ret = func<Rtype,double> (R, __VA_ARGS__); break;               \
    default:                                                            \
        (R).error ("%s: Unsupported pixel data format '%s'", name, Atype); \
        ret = false;                                                    \
    }

// Macro to call a type-specialzed version func<Rtype,Atype>(R,...).
#define OIIO_DISPATCH_TYPES2(ret,name,func,Rtype,Atype,R,...)           \
    switch (Rtype.basetype) {                                           \
    case TypeDesc::FLOAT :                                              \
        OIIO_DISPATCH_TYPES2_HELP(ret,name,func,float,Atype,R,__VA_ARGS__); \
        break;                                                          \
    case TypeDesc::UINT8 :                                              \
        OIIO_DISPATCH_TYPES2_HELP(ret,name,func,unsigned char,Atype,R,__VA_ARGS__); \
        break;                                                          \
    case TypeDesc::HALF  :                                              \
        OIIO_DISPATCH_TYPES2_HELP(ret,name,func,half,Atype,R,__VA_ARGS__);  \
        break;                                                          \
    case TypeDesc::UINT16:                                              \
        OIIO_DISPATCH_TYPES2_HELP(ret,name,func,unsigned short,Atype,R,__VA_ARGS__); \
        break;                                                          \
    case TypeDesc::INT8:                                                \
        OIIO_DISPATCH_TYPES2_HELP(ret,name,func,char,Atype,R,__VA_ARGS__);  \
        break;                                                          \
    case TypeDesc::INT16:                                               \
        OIIO_DISPATCH_TYPES2_HELP(ret,name,func,short,Atype,R,__VA_ARGS__); \
        break;                                                          \
    case TypeDesc::UINT:                                                \
        OIIO_DISPATCH_TYPES2_HELP(ret,name,func,unsigned int,Atype,R,__VA_ARGS__); \
        break;                                                          \
    case TypeDesc::INT:                                                 \
        OIIO_DISPATCH_TYPES2_HELP(ret,name,func,int,Atype,R,__VA_ARGS__); \
        break;                                                          \
    case TypeDesc::DOUBLE:                                              \
        OIIO_DISPATCH_TYPES2_HELP(ret,name,func,double,Atype,R,__VA_ARGS__);\
        break;                                                          \
    default:                                                            \
        (R).error ("%s: Unsupported pixel data format '%s'", name, Rtype); \
        ret = false;                                                    \
    }


// Macro to call a type-specialzed version func<type>(R,...) for
// the most common types, will auto-convert the rest to float.
#define OIIO_DISPATCH_COMMON_TYPES(ret,name,func,type,R,...)            \
    switch (type.basetype) {                                            \
    case TypeDesc::FLOAT :                                              \
        ret = func<float> (R, __VA_ARGS__); break;                      \
    case TypeDesc::UINT8 :                                              \
        ret = func<unsigned char> (R, __VA_ARGS__); break;              \
    case TypeDesc::HALF  :                                              \
        ret = func<half> (R, __VA_ARGS__); break;                       \
    case TypeDesc::UINT16:                                              \
        ret = func<unsigned short> (R, __VA_ARGS__); break;             \
    default: {                                                          \
        /* other types: punt and convert to float, then copy back */    \
        ImageBuf Rtmp;                                                  \
        if ((R).initialized())                                          \
            Rtmp.copy (R, TypeDesc::FLOAT);                             \
        ret = func<float> (Rtmp, __VA_ARGS__);                          \
        if (ret)                                                        \
            (R).copy (Rtmp);                                            \
        else                                                            \
            (R).error ("%s", Rtmp.geterror());                          \
        }                                                               \
    }

// Helper, do not call from the outside world.
#define OIIO_DISPATCH_COMMON_TYPES2_HELP(ret,name,func,Rtype,Atype,R,A,...) \
    switch (Atype.basetype) {                                           \
    case TypeDesc::FLOAT :                                              \
        ret = func<Rtype,float> (R, A, __VA_ARGS__); break;             \
    case TypeDesc::UINT8 :                                              \
        ret = func<Rtype,unsigned char> (R, A, __VA_ARGS__); break;     \
    case TypeDesc::HALF  :                                              \
        ret = func<Rtype,half> (R, A, __VA_ARGS__); break;              \
    case TypeDesc::UINT16:                                              \
        ret = func<Rtype,unsigned short> (R, A, __VA_ARGS__); break;    \
    default: {                                                          \
        /* other types: punt and convert to float, then copy back */    \
        ImageBuf Atmp;                                                  \
        Atmp.copy (A, TypeDesc::FLOAT);                                 \
        ret = func<Rtype,float> (R, Atmp, __VA_ARGS__);                 \
        }                                                               \
    }

// Macro to call a type-specialzed version func<Rtype,Atype>(R,A,...) for
// the most common types, will auto-convert the rest to float.
#define OIIO_DISPATCH_COMMON_TYPES2(ret,name,func,Rtype,Atype,R,A,...)  \
    switch (Rtype.basetype) {                                           \
    case TypeDesc::FLOAT :                                              \
        OIIO_DISPATCH_COMMON_TYPES2_HELP(ret,name,func,float,Atype,R,A,__VA_ARGS__); \
        break;                                                          \
    case TypeDesc::UINT8 :                                              \
        OIIO_DISPATCH_COMMON_TYPES2_HELP(ret,name,func,unsigned char,Atype,R,A,__VA_ARGS__); \
        break;                                                          \
    case TypeDesc::HALF  :                                              \
        OIIO_DISPATCH_COMMON_TYPES2_HELP(ret,name,func,half,Atype,R,A,__VA_ARGS__); \
        break;                                                          \
    case TypeDesc::UINT16:                                              \
        OIIO_DISPATCH_COMMON_TYPES2_HELP(ret,name,func,unsigned short,Atype,R,A,__VA_ARGS__); \
        break;                                                          \
    default: {                                                          \
        /* other types: punt and convert to float, then copy back */    \
        ImageBuf Rtmp;                                                  \
        if ((R).initialized())                                          \
            Rtmp.copy (R, TypeDesc::FLOAT);                             \
        OIIO_DISPATCH_COMMON_TYPES2_HELP(ret,name,func,float,Atype,Rtmp,A,__VA_ARGS__); \
        if (ret)                                                        \
            (R).copy (Rtmp);                                            \
        else                                                            \
            (R).error ("%s", Rtmp.geterror());                          \
        }                                                               \
    }


// Helper, do not call from the outside world.
#define OIIO_DISPATCH_COMMON_TYPES3_HELP2(ret,name,func,Rtype,Atype,Btype,R,A,B,...) \
    switch (Rtype.basetype) {                                           \
    case TypeDesc::FLOAT :                                              \
        ret = func<float,Atype,Btype> (R,A,B,__VA_ARGS__); break;       \
    case TypeDesc::UINT8 :                                              \
        ret = func<unsigned char,Atype,Btype> (R,A,B,__VA_ARGS__); break;  \
    case TypeDesc::HALF  :                                              \
        ret = func<half,Atype,Btype> (R,A,B,__VA_ARGS__); break;        \
    case TypeDesc::UINT16:                                              \
        ret = func<unsigned short,Atype,Btype> (R,A,B,__VA_ARGS__); break;  \
    default: {                                                          \
        /* other types: punt and convert to float, then copy back */    \
        ImageBuf Rtmp;                                                  \
        if ((R).initialized())                                          \
            Rtmp.copy (R, TypeDesc::FLOAT);                             \
        ret = func<float,Atype,Btype> (R,A,B,__VA_ARGS__);              \
        if (ret)                                                        \
            (R).copy (Rtmp);                                            \
        else                                                            \
            (R).error ("%s", Rtmp.geterror());                          \
        }                                                               \
    }

// Helper, do not call from the outside world.
#define OIIO_DISPATCH_COMMON_TYPES3_HELP(ret,name,func,Rtype,Atype,Btype,R,A,B,...) \
    switch (Btype.basetype) {                                           \
    case TypeDesc::FLOAT :                                              \
        OIIO_DISPATCH_COMMON_TYPES3_HELP2(ret,name,func,Rtype,Atype,float,R,A,B,__VA_ARGS__); \
        break;                                                          \
    case TypeDesc::UINT8 :                                              \
        OIIO_DISPATCH_COMMON_TYPES3_HELP2(ret,name,func,Rtype,Atype,unsigned char,R,A,B,__VA_ARGS__); \
        break;                                                          \
    case TypeDesc::HALF :                                               \
        OIIO_DISPATCH_COMMON_TYPES3_HELP2(ret,name,func,Rtype,Atype,half,R,A,B,__VA_ARGS__); \
        break;                                                          \
    case TypeDesc::UINT16 :                                             \
        OIIO_DISPATCH_COMMON_TYPES3_HELP2(ret,name,func,Rtype,Atype,unsigned short,R,A,B,__VA_ARGS__); \
        break;                                                          \
    default: {                                                          \
        /* other types: punt and convert to float */                    \
        ImageBuf Btmp;                                                  \
        Btmp.copy (B, TypeDesc::FLOAT);                                 \
        OIIO_DISPATCH_COMMON_TYPES3_HELP2(ret,name,func,Rtype,Atype,float,R,A,Btmp,__VA_ARGS__); \
        }                                                               \
    }

// Macro to call a type-specialzed version func<Rtype,Atype,Btype>(R,A,B,...)
// the most common types, will auto-convert the rest to float.
#define OIIO_DISPATCH_COMMON_TYPES3(ret,name,func,Rtype,Atype,Btype,R,A,B,...)  \
    switch (Atype.basetype) {                                           \
    case TypeDesc::FLOAT :                                              \
        OIIO_DISPATCH_COMMON_TYPES3_HELP(ret,name,func,Rtype,float,Btype,R,A,B,__VA_ARGS__); \
        break;                                                          \
    case TypeDesc::UINT8 :                                              \
        OIIO_DISPATCH_COMMON_TYPES3_HELP(ret,name,func,Rtype,unsigned char,Btype,R,A,B,__VA_ARGS__); \
        break;                                                          \
    case TypeDesc::HALF  :                                              \
        OIIO_DISPATCH_COMMON_TYPES3_HELP(ret,name,func,Rtype,half,Btype,R,A,B,__VA_ARGS__); \
        break;                                                          \
    case TypeDesc::UINT16:                                              \
        OIIO_DISPATCH_COMMON_TYPES3_HELP(ret,name,func,Rtype,unsigned short,Btype,R,A,B,__VA_ARGS__); \
        break;                                                          \
    default:                                                            \
        /* other types: punt and convert to float */                    \
        ImageBuf Atmp;                                                  \
        Atmp.copy (A, TypeDesc::FLOAT);                                 \
        OIIO_DISPATCH_COMMON_TYPES3_HELP(ret,name,func,Rtype,float,Btype,R,Atmp,B,__VA_ARGS__); \
    }



}  // end namespace ImageBufAlgo


OIIO_NAMESPACE_END

#endif // OPENIMAGEIO_IMAGEBUFALGO_UTIL_H
