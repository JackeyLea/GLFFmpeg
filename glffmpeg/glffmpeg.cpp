/*
* Main source for the glFFmpeg library
* Copyright ( c ) 2006 Marco Ippolito
*
* This file is part of glFFmpeg.
*/

#include "glffmpeg.h"

#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#else
#include <time.h>
#endif

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <map>
#include <string>

static bool s_bInitialized = false;

#ifdef WIN32

BOOL APIENTRY DllMain( HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved )
{
	switch( fdwReason )
	{
	    case DLL_PROCESS_ATTACH:
	    case DLL_THREAD_ATTACH:
	    case DLL_THREAD_DETACH:
	    case DLL_PROCESS_DETACH:
		    break;
	}

    return TRUE;
}

#endif //WIN32

class ffmpegHelper
{
public:

    /**
     * Default constructor
     */
    ffmpegHelper() : 
        m_pAVIFile(NULL),
        m_videoContext(NULL),
        m_status(0),
        m_width(0),
        m_height(0),
        m_bShutdownRequested(false)
    {}

    /**
     * Default destructor
     */
    ~ffmpegHelper()
    {
        m_bShutdownRequested = true;

        // Close each codec
        if( m_pAVIFile )
        {
            avcodec_close( m_pAVIFile->codec );
            free( m_yuvFrame->data[0] );
            av_free( m_yuvFrame );
            av_free( m_rgbFrame );
            free( m_videoBuffer );
        }

        // Write the trailer, if any
        av_write_trailer( m_videoContext );

        // Free the streams
        int i = 0;
        for( ; i < m_videoContext->nb_streams; i++ )
        {
            av_freep( &m_videoContext->streams[i]->codec );
            av_freep( &m_videoContext->streams[i] );
        }

        if( !( m_videoFormat->flags & AVFMT_NOFILE ) )
        {
            // close the output file
            url_fclose( &m_videoContext->pb );
        }

        // free the stream
        av_free( m_videoContext );
    }

    /**
     * Configure a stream for recording purposes. This method will try to set 
     * up a recording stream at the specified filename with the specified frame 
     * rate and the specified dimensions.
     */
    int configure( const char* fileName, int fpsRate, int width, int height,
                   void* imageBuffer )
    {
        m_width = width;
        m_height = height;

        if( ( m_width % 160 ) || ( m_height % 160 ) )
        {
            printf( "ffmpegHelper::configure: window dimensions are not "
                "divisible by 160, some codecs may not like this!\n" );
        }

        // Auto detect the output format from the name. default is  mpeg.
        m_videoFormat = guess_format( NULL, fileName, NULL );

        if( !m_videoFormat )
        {
            printf( "Could not deduce output format from file extension. Defaulting to MPEG.\n" );
            m_videoFormat = guess_format( "mpeg", NULL, NULL );
        }

        if( !m_videoFormat )
        {
            printf( "ffmpegHelper::configure: Unable to locate a suitable encoding format.\n" );
            m_status = 1;
            return m_status;
        }

        // Allocate the output media context.
        m_videoContext = av_alloc_format_context();

        if( !m_videoContext )
        {
            printf( "ffmpegHelper::configure: Error allocating video context.\n" );
            m_status = 2;
            return m_status;
        }

        m_videoContext->oformat = m_videoFormat;

#ifdef _WIN32
        _snprintf( m_videoContext->filename, sizeof( m_videoContext->filename ), "%s", fileName );
#else
        snprintf( m_videoContext->filename, sizeof( m_videoContext->filename ), "%s", fileName );
#endif

        // video stream using the default format codec and initialize the codec
        m_pAVIFile= NULL;

        if( m_videoFormat->video_codec != CODEC_ID_NONE )
        {
            m_pAVIFile = av_new_stream( m_videoContext, 0 );

            if( !m_pAVIFile )
            {
                printf( "ffmpegHelper::configure: Error allocating video stream.\n" );
                m_status = 3;
                return m_status;
            }

            m_codecContext = m_pAVIFile->codec;
            m_codecContext->codec_id = m_videoFormat->video_codec;
            m_codecContext->codec_type = CODEC_TYPE_VIDEO;
            m_codecContext->bit_rate = 7500000;
            m_codecContext->width = m_width;
            m_codecContext->height = m_height;
            m_codecContext->time_base.den = fpsRate;
            m_codecContext->time_base.num = 1;
            m_codecContext->gop_size = 12; 
            m_codecContext->pix_fmt = PIX_FMT_YUV420P;

            if( m_codecContext->codec_id == CODEC_ID_MPEG1VIDEO )
                m_codecContext->mb_decision = 2;

            // Some formats want stream headers to be separate
            if( !strcmp( m_videoContext->oformat->name, "mp4" ) || 
                !strcmp( m_videoContext->oformat->name, "mov" ) || 
                !strcmp( m_videoContext->oformat->name, "3gp" ) )
            {
                m_codecContext->flags |= CODEC_FLAG_GLOBAL_HEADER;
            }
        }

        // Set the output parameters
        if( av_set_parameters( m_videoContext, NULL ) < 0 )
        {
            printf( "ffmpegHelper::configure: Invalid output parameters.\n" );
            m_status = 4;
            return m_status;
        }

        // Print encoding format information to the console
        dump_format( m_videoContext, 0, fileName, 1 );

        if( m_pAVIFile )
        {
            // Find the video encoder
            AVCodec* codec = avcodec_find_encoder( m_codecContext->codec_id );

            if( !codec )
            {
                printf( "ffmpegHelper::configure: Codec not found.\n" );
                m_status = 5;
                return m_status;
            }

            // Open the codec
            if( avcodec_open( m_codecContext, codec ) < 0 )
            {
                printf( "ffmpegHelper::configure: Could not open codec.\n" );
                m_status = 6;
                return m_status;
            }

            m_videoBuffer = NULL;

            if( !( m_videoContext->oformat->flags & AVFMT_RAWPICTURE ) )
            {
                // Allocate output buffer
                m_videoBuffer = (uint8_t*) malloc(200000);
            }

            // Allocate the video frames
            uint8_t *picture_buf;
            int size;

            m_yuvFrame = avcodec_alloc_frame();

            if( !m_yuvFrame )
            {
                printf( "ffmpegHelper::configure: Error allocating video frame.\n" );
                m_status = 7;
                return m_status;
            }

            size = avpicture_get_size( m_codecContext->pix_fmt, m_width, m_height );
            picture_buf = (uint8_t*) malloc( size );

            if( !picture_buf )
            {
                printf( "ffmpegHelper::configure: Error allocating video frame.\n" );
                m_status = 8;
                return m_status;
            }

            avpicture_fill( (AVPicture*)m_yuvFrame, picture_buf,
                m_codecContext->pix_fmt, m_width, m_height );

            m_rgbFrame = avcodec_alloc_frame();

            if( !m_rgbFrame )
            {
                printf( "ffmpegHelper::configure: Error allocating video frame.\n" );
                m_status = 9;
                return m_status;
            }

            size = avpicture_get_size( PIX_FMT_RGB24, m_width, m_height );

            avpicture_fill( (AVPicture*)m_rgbFrame, (uint8_t*)imageBuffer,
                            PIX_FMT_RGB24, m_width, m_height );
        }

        // Open the output file, if needed.
        if( !( m_videoFormat->flags & AVFMT_NOFILE ) )
        {
            if( url_fopen( &m_videoContext->pb, fileName, URL_WRONLY ) < 0 )
            {
                printf( "ffmpegHelper::configure: Unable to open output file <%s>.\n", fileName );
                m_status = 10;
                return m_status;
            }
        }

        // Write the stream header, if any
        av_write_header( m_videoContext );

        m_status = 0;
        return m_status;
    }

    /**
     * Retrieves the current status of the AVI library.
     *
     * Possible status error codes for return value
     *
     * 0 Success - status normal.
     * 1 ffmpegHelper::configure: Unable to locate a suitable encoding format.
     * 2 ffmpegHelper::configure: Error allocating video context.
     * 3 ffmpegHelper::configure: Error allocating video stream.
     * 4 ffmpegHelper::configure: Invalid output parameters.
     * 5 ffmpegHelper::configure: Codec not found.
     * 6 ffmpegHelper::configure: Could not open codec.
     * 7 ffmpegHelper::configure: Error allocating video frame.
     * 8 ffmpegHelper::configure: Unable to open output file <file_name>,
     * 9 ffmpegHelper::encodeFrame: You need to create a stream first.
     */
    inline int getStatus( void ) const { return m_status; }

    /**
     * Captures a single frame of video to the stream
     */
    int encodeFrame()
    {
        if( m_bShutdownRequested )
        {
            m_status = 0;
            return m_status;
        }

        if( !m_pAVIFile )
        {
            printf( "ffmpegHelper::encodeFrame: You need to create a stream first.\n" );
            m_status = 11;
            return m_status;
        }

        // convert RGB to YUV
        img_convert( ( AVPicture * ) m_yuvFrame, m_codecContext->pix_fmt,
                     ( AVPicture * ) m_rgbFrame, PIX_FMT_RGB24, m_width, m_height );

        // flip the YUV frame upside down
        unsigned char* s;
        unsigned char* d;
        static unsigned char  b[24000];

        for ( s= m_yuvFrame->data[0], d= m_yuvFrame->data[1]-m_yuvFrame->linesize[0];
            s < d; s+= m_yuvFrame->linesize[0], d-= m_yuvFrame->linesize[0] )
        {
            memcpy( b, s, m_yuvFrame->linesize[0] );
            memcpy( s, d, m_yuvFrame->linesize[0] );
            memcpy( d, b, m_yuvFrame->linesize[0] );
        }

        for ( s= m_yuvFrame->data[1], d= m_yuvFrame->data[2]-m_yuvFrame->linesize[2];
            s < d; s+= m_yuvFrame->linesize[1], d-= m_yuvFrame->linesize[1] )
        {
            memcpy( b, s, m_yuvFrame->linesize[1] );
            memcpy( s, d, m_yuvFrame->linesize[1] );
            memcpy( d, b, m_yuvFrame->linesize[1] );
        }

        for ( s= m_yuvFrame->data[2], d= m_yuvFrame->data[2]+
            ( m_yuvFrame->data[2]-m_yuvFrame->data[1]-m_yuvFrame->linesize[2] );
            s < d; s+= m_yuvFrame->linesize[2], d-= m_yuvFrame->linesize[2] )
        {
            memcpy( b, s, m_yuvFrame->linesize[2] );
            memcpy( s, d, m_yuvFrame->linesize[2] );
            memcpy( d, b, m_yuvFrame->linesize[2] );
        }

        // Encode the YUV frame.
        size_t out_size = avcodec_encode_video( m_codecContext, m_videoBuffer, 
                                                m_width*m_height*30, m_yuvFrame );

        // If zero size, it means the image was buffered.
        if( out_size > 0 )
        {
            AVPacket pkt;
            av_init_packet( &pkt );

            if( m_codecContext->coded_frame->key_frame )
                pkt.flags |= PKT_FLAG_KEY;
            pkt.stream_index= m_pAVIFile->index;
            pkt.data= m_videoBuffer;
            pkt.size= out_size;

            // Write the compressed frame in the media file.
            av_write_frame( m_videoContext, &pkt );
        }

        m_status = 0;
        return m_status;
    }

private:

    int m_status; 
    int m_width;
    int m_height;
    bool m_bShutdownRequested;
    AVStream* m_pAVIFile;
    AVFormatContext* m_videoContext;
    AVOutputFormat* m_videoFormat;
    AVCodecContext* m_codecContext;
    AVFrame* m_rgbFrame;
    AVFrame* m_yuvFrame;
    uint8_t* m_videoBuffer;
};

typedef std::map<std::string, ffmpegHelper* > ffmpegHelpers;
static ffmpegHelpers s_ffmpegHelpers;

#ifdef __cplusplus
extern "C" 
{
#endif

int __cdecl initializeGLFFMPEG()
{
    av_register_all();
    s_bInitialized = true;

    return 1;
}

int __cdecl shutdownGLFFMPEG()
{
    // Iterate through ffmpegHelpers and delete them
    ffmpegHelpers::iterator it = s_ffmpegHelpers.begin();
    ffmpegHelpers::iterator ite = s_ffmpegHelpers.end();

    for( ; it != ite; ++it )
        delete it->second;

    s_ffmpegHelpers.clear();
    s_bInitialized = false;

    return 1;
}

int __cdecl initializeStream( const char* streamName, int fpsRate, 
                              int width, int height, void* imageBuffer )
{
    if( streamName == NULL || fpsRate == 0 || 
        imageBuffer == NULL || width == 0 || height == 0 )
    {
        printf( "initializeStream : invalid parameter provided\n" );
        return 1;
    }

    ffmpegHelper* helper = new ffmpegHelper();
    
    int ret = helper->configure( streamName, fpsRate, width, height, imageBuffer );

    if( ret != 0 )
        return ret;

    s_ffmpegHelpers.insert( ffmpegHelpers::value_type( streamName, helper ) );

    return 0;
}

int __cdecl encodeFrame( const char* streamName )
{
    // Find the appropriate ffmpegHelper
    ffmpegHelpers::iterator it = s_ffmpegHelpers.find( streamName );

    if( it == s_ffmpegHelpers.end() )
        return 1;

    // Call encodeFrame
    it->second->encodeFrame();

    return 0;
}

int __cdecl shutdownStream( const char* streamName )
{
    // Find the appropriate ffmpegHelper
    ffmpegHelpers::iterator it = s_ffmpegHelpers.find( streamName );

    if( it == s_ffmpegHelpers.end() )
        return 1;

    // Call its destructor
    delete it->second;

    // Remove it from the ffmpegHelpers list
    s_ffmpegHelpers.erase( it );

    return 0;
}

int __cdecl getStatus( const char* streamName )
{
    // Find the appropriate ffmpegHelper
    ffmpegHelpers::iterator it = s_ffmpegHelpers.find( streamName );

    if( it == s_ffmpegHelpers.end() )
        return 1;

    return (*it).second->getStatus();
}

#ifdef __cplusplus
}
#endif
