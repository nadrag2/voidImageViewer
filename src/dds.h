//
// Copyright 2026 voidImageViewer contributors
// 
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
//
// dds layer

#ifdef __cplusplus
extern "C" {
#endif

// returns non-zero if the stream starts with the DDS magic number.
int dds_is_dds(IStream *stream);

// load the first surface of a DDS file.
// same callback interface as webp_load.
// the pixels passed to frame_callback are RGBA (4 bytes per pixel, top-down) and may be modified by the callback.
int dds_load(IStream *stream,void *user_data,int (*info_callback)(void *user_data,DWORD frame_count,DWORD wide,DWORD high,int has_alpha),int (*frame_callback)(void *user_data,BYTE *pixels,int timestamp));

// decode the first surface of a DDS file in memory.
// returns an RGBA (4 bytes per pixel, top-down) buffer or NULL if the file is invalid or unsupported.
// free the returned buffer with dds_free.
BYTE *dds_decode(const BYTE *data,SIZE_T size,DWORD *out_wide,DWORD *out_high,int *out_has_alpha);
void dds_free(BYTE *pixels);

#ifdef __cplusplus
}
#endif

