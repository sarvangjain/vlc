/*****************************************************************************
 * file.c: file input (file: access plug-in)
 *****************************************************************************
 * Copyright (C) 2001, 2002 VideoLAN
 * $Id: file.c,v 1.18 2003/04/16 11:47:08 gbazin Exp $
 *
 * Authors: Christophe Massiot <massiot@via.ecp.fr>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111, USA.
 *****************************************************************************/

/*****************************************************************************
 * Preamble
 *****************************************************************************/
#include <vlc/vlc.h>
#include <vlc/input.h>

#include <stdlib.h>
#include <string.h>
#ifdef HAVE_SYS_TYPES_H
#   include <sys/types.h>
#endif
#ifdef HAVE_SYS_STAT_H
#   include <sys/stat.h>
#endif
#ifdef HAVE_ERRNO_H
#   include <errno.h>
#endif
#ifdef HAVE_FCNTL_H
#   include <fcntl.h>
#endif

#ifdef HAVE_UNISTD_H
#   include <unistd.h>
#elif defined( WIN32 ) && !defined( UNDER_CE )
#   include <io.h>
#endif

/*****************************************************************************
 * Exported prototypes
 *****************************************************************************/
static int     Open   ( vlc_object_t * );
static void    Close  ( vlc_object_t * );

static void    Seek   ( input_thread_t *, off_t );
static ssize_t Read   ( input_thread_t *, byte_t *, size_t );

/*****************************************************************************
 * Module descriptor
 *****************************************************************************/
#define CACHING_TEXT N_("caching value in ms")
#define CACHING_LONGTEXT N_( \
    "Allows you to modify the default caching value for file streams. This " \
    "value should be set in miliseconds units." )

vlc_module_begin();
    set_description( _("Standard filesystem file input") );
    add_category_hint( N_("file"), NULL, VLC_TRUE );
    add_integer( "file-caching", DEFAULT_PTS_DELAY / 1000, NULL, CACHING_TEXT, CACHING_LONGTEXT, VLC_TRUE );
    set_capability( "access", 50 );
    add_shortcut( "file" );
    add_shortcut( "stream" );
    add_shortcut( "kfir" );
    set_callbacks( Open, Close );
vlc_module_end();
 
/*****************************************************************************
 * _input_socket_t: private access plug-in data, modified to add private
 *                  fields
 *****************************************************************************/
typedef struct _input_socket_s
{
    input_socket_t      _socket;

    unsigned int        i_nb_reads;
    vlc_bool_t          b_kfir;
} _input_socket_t;

/*****************************************************************************
 * Open: open the file
 *****************************************************************************/
static int Open( vlc_object_t *p_this )
{
    input_thread_t *    p_input = (input_thread_t *)p_this;
    char *              psz_name = p_input->psz_name;
#ifdef HAVE_SYS_STAT_H
    int                 i_stat;
    struct stat         stat_info;
#endif
    _input_socket_t *   p_access_data;
    vlc_bool_t          b_stdin, b_kfir = 0;

    p_input->i_mtu = 0;

    b_stdin = psz_name[0] == '-' && psz_name[1] == '\0';

#ifdef HAVE_SYS_STAT_H
    if( !b_stdin && (i_stat = stat( psz_name, &stat_info )) == (-1) )
    {
#   ifdef HAVE_ERRNO_H
        msg_Warn( p_input, "cannot stat() file `%s' (%s)",
                  psz_name, strerror(errno));
#   else
        msg_Warn( p_input, "cannot stat() file `%s'", psz_name );
#   endif
        return VLC_EGENERIC;
    }
#endif

    p_input->pf_read = Read;
    p_input->pf_set_program = input_SetProgram;
    p_input->pf_set_area = NULL;
    p_input->pf_seek = Seek;

    vlc_mutex_lock( &p_input->stream.stream_lock );

    if( *p_input->psz_access && !strncmp( p_input->psz_access, "stream", 7 ) )
    {
        /* stream:%s */
        p_input->stream.b_pace_control = 0;
        p_input->stream.b_seekable = 0;
        p_input->stream.p_selected_area->i_size = 0;
    }
    else if( *p_input->psz_access &&
             !strncmp( p_input->psz_access, "kfir", 7 ) )
    {
        /* stream:%s */
        p_input->stream.b_pace_control = 0;
        p_input->stream.b_seekable = 0;
        p_input->stream.p_selected_area->i_size = 0;
        b_kfir = 1;
    }
    else
    {
        /* file:%s or %s */
        p_input->stream.b_pace_control = 1;

        if( b_stdin )
        {
            p_input->stream.b_seekable = 0;
            p_input->stream.p_selected_area->i_size = 0;
        }
#ifdef UNDER_CE
        else if( VLC_TRUE )
        {
            /* We'll update i_size after it's been opened */
            p_input->stream.b_seekable = 1;
        }
#elif defined( HAVE_SYS_STAT_H )
        else if( S_ISREG(stat_info.st_mode) || S_ISCHR(stat_info.st_mode)
                  || S_ISBLK(stat_info.st_mode) )
        {
            p_input->stream.b_seekable = 1;
            p_input->stream.p_selected_area->i_size = stat_info.st_size;
        }
        else if( S_ISFIFO(stat_info.st_mode)
#   if !defined( SYS_BEOS ) && !defined( WIN32 )
                  || S_ISSOCK(stat_info.st_mode)
#   endif
               )
        {
            p_input->stream.b_seekable = 0;
            p_input->stream.p_selected_area->i_size = 0;
        }
#endif
        else
        {
            vlc_mutex_unlock( &p_input->stream.stream_lock );
            msg_Err( p_input, "unknown file type for `%s'", psz_name );
            return VLC_EGENERIC;
        }
    }
 
    p_input->stream.p_selected_area->i_tell = 0;
    p_input->stream.i_method = INPUT_METHOD_FILE;
    vlc_mutex_unlock( &p_input->stream.stream_lock );
 
    msg_Dbg( p_input, "opening file `%s'", psz_name );
    p_access_data = malloc( sizeof(_input_socket_t) );
    p_input->p_access_data = (void *)p_access_data;
    if( p_access_data == NULL )
    {
        msg_Err( p_input, "out of memory" );
        return VLC_ENOMEM;
    }

    p_access_data->i_nb_reads = 0;
    p_access_data->b_kfir = b_kfir;
    if( b_stdin )
    {
        p_access_data->_socket.i_handle = 0;
    }
    else
    {
#ifdef UNDER_CE
        wchar_t psz_filename[MAX_PATH];
        MultiByteToWideChar( CP_ACP, 0, psz_name, -1, psz_filename, MAX_PATH );

        p_access_data->_socket.i_handle = (int)CreateFile( psz_filename,
            GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 
            FILE_ATTRIBUTE_NORMAL, NULL );

        if( (HANDLE)p_access_data->_socket.i_handle == INVALID_HANDLE_VALUE )
        {
            msg_Err( p_input, "cannot open file %s", psz_name );
            free( p_access_data );
            return VLC_EGENERIC;
        }
        p_input->stream.p_selected_area->i_size =
                GetFileSize( (HANDLE)p_access_data->_socket.i_handle, NULL );
#else
        p_access_data->_socket.i_handle = open( psz_name,
                                                O_NONBLOCK /*| O_LARGEFILE*/ );
        if( p_access_data->_socket.i_handle == -1 )
        {
#   ifdef HAVE_ERRNO_H
            msg_Err( p_input, "cannot open file %s (%s)", psz_name,
                              strerror(errno) );
#   else
            msg_Err( p_input, "cannot open file %s", psz_name );
#   endif
            free( p_access_data );
            return VLC_EGENERIC;
        }
#endif
    }

    if ( p_input->stream.b_seekable
          && !p_input->stream.p_selected_area->i_size )
    {
        msg_Err( p_input, "file %s is empty, aborting", psz_name );
        free( p_access_data );
        return VLC_EGENERIC;
    }

    /* Update default_pts to a suitable value for file access */
    p_input->i_pts_delay = config_GetInt( p_input, "file-caching" ) * 1000;

    return VLC_SUCCESS;
}

/*****************************************************************************
 * Close: close the target
 *****************************************************************************/
static void Close( vlc_object_t * p_this )
{
    input_thread_t * p_input = (input_thread_t *)p_this;
    input_socket_t * p_access_data = (input_socket_t *)p_input->p_access_data;

    msg_Info( p_input, "closing `%s/%s://%s'", 
              p_input->psz_access, p_input->psz_demux, p_input->psz_name );
 
#ifdef UNDER_CE
    CloseHandle( (HANDLE)p_access_data->i_handle );
#else
    close( p_access_data->i_handle );
#endif

    free( p_access_data );
}

/*****************************************************************************
 * Read: standard read on a file descriptor.
 *****************************************************************************/
static ssize_t Read( input_thread_t * p_input, byte_t * p_buffer, size_t i_len )
{
    _input_socket_t * p_access_data = (_input_socket_t *)p_input->p_access_data;
    ssize_t i_ret;
 
#ifdef UNDER_CE
    if( !ReadFile( (HANDLE)p_access_data->_socket.i_handle, p_buffer, i_len,
                   (LPDWORD)&i_ret, NULL ) )
    {
        i_ret = -1;
    }
#else
#ifndef WIN32
    if ( !p_input->stream.b_pace_control )
    {
        if ( !p_access_data->b_kfir )
        {
            /* Find if some data is available. This won't work under Windows. */
            struct timeval  timeout;
            fd_set          fds;

            /* Initialize file descriptor set */
            FD_ZERO( &fds );
            FD_SET( p_access_data->_socket.i_handle, &fds );

            /* We'll wait 0.5 second if nothing happens */
            timeout.tv_sec = 0;
            timeout.tv_usec = 500000;

            /* Find if some data is available */
            while( (i_ret = select( p_access_data->_socket.i_handle + 1, &fds,
                                    NULL, NULL, &timeout )) == 0
                    || (i_ret < 0 && errno == EINTR) )
            {
                FD_ZERO( &fds );
                FD_SET( p_access_data->_socket.i_handle, &fds );
                timeout.tv_sec = 0;
                timeout.tv_usec = 500000;

                if( p_input->b_die || p_input->b_error )
                {
                    return 0;
                }
            }

            if( i_ret < 0 )
            {
                msg_Err( p_input, "select error (%s)", strerror(errno) );
                return -1;
            }

            i_ret = read( p_access_data->_socket.i_handle, p_buffer, i_len );
        }
        else
        {
            /* b_kfir ; work around a buggy poll() driver implementation */
            while ( (i_ret = read( p_access_data->_socket.i_handle, p_buffer,
                                   i_len )) == 0 &&
                      !p_input->b_die && !p_input->b_error )
            {
                msleep(INPUT_ERROR_SLEEP);
            }
        }
    }
    else
#   endif
    {
        /* b_pace_control || WIN32 */
        i_ret = read( p_access_data->_socket.i_handle, p_buffer, i_len );
    }
#endif

    if( i_ret < 0 )
    {
#ifdef HAVE_ERRNO_H
        if ( errno != EINTR && errno != EAGAIN )
            msg_Err( p_input, "read failed (%s)", strerror(errno) );
#else
        msg_Err( p_input, "read failed" );
#endif

        /* Delay a bit to avoid consuming all the CPU. This is particularly
         * useful when reading from an unconnected FIFO. */
        msleep( INPUT_ERROR_SLEEP );
    }
 
    p_access_data->i_nb_reads++;
#ifdef HAVE_SYS_STAT_H
    if ( p_input->stream.p_selected_area->i_size != 0
            && (p_access_data->i_nb_reads % INPUT_FSTAT_NB_READS) == 0 )
    {
        struct stat stat_info;
        if ( fstat( p_access_data->_socket.i_handle, &stat_info ) == -1 )
        {
#   ifdef HAVE_ERRNO_H
            msg_Warn( p_input, "couldn't stat again the file (%s)",
                      strerror(errno) );
#   else
            msg_Warn( p_input, "couldn't stat again the file" );
#   endif
        }
        else if ( p_input->stream.p_selected_area->i_size != stat_info.st_size )
        {
            p_input->stream.p_selected_area->i_size = stat_info.st_size;
            p_input->stream.b_changed = 1;
        }
    }
#endif

    return i_ret;
}

/*****************************************************************************
 * Seek: seek to a specific location in a file
 *****************************************************************************/
static void Seek( input_thread_t * p_input, off_t i_pos )
{
#define S p_input->stream
    input_socket_t * p_access_data = (input_socket_t *)p_input->p_access_data;

#if defined( WIN32 ) && !defined( UNDER_CE )
    _lseeki64( p_access_data->i_handle, i_pos, SEEK_SET );
#else
    lseek( p_access_data->i_handle, i_pos, SEEK_SET );
#endif

    vlc_mutex_lock( &S.stream_lock );
    S.p_selected_area->i_tell = i_pos;
    if( S.p_selected_area->i_tell > S.p_selected_area->i_size )
    {
        msg_Err( p_input, "seeking too far" );
        S.p_selected_area->i_tell = S.p_selected_area->i_size;
    }
    else if( S.p_selected_area->i_tell < 0 )
    {
        msg_Err( p_input, "seeking too early" );
        S.p_selected_area->i_tell = 0;
    }
    vlc_mutex_unlock( &S.stream_lock );
#undef S
}

