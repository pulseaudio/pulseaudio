#ifndef foonativecommonhfoo
#define foonativecommonhfoo

/* $Id$ */

/***
  This file is part of polypaudio.
 
  polypaudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published
  by the Free Software Foundation; either version 2 of the License,
  or (at your option) any later version.
 
  polypaudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.
 
  You should have received a copy of the GNU General Public License
  along with polypaudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

enum {
    PA_COMMAND_ERROR,
    PA_COMMAND_TIMEOUT, /* pseudo command */
    PA_COMMAND_REPLY,
    PA_COMMAND_CREATE_PLAYBACK_STREAM,
    PA_COMMAND_DELETE_PLAYBACK_STREAM,
    PA_COMMAND_CREATE_RECORD_STREAM,
    PA_COMMAND_DELETE_RECORD_STREAM,
    PA_COMMAND_EXIT,
    PA_COMMAND_REQUEST,
    PA_COMMAND_AUTH,
    PA_COMMAND_SET_NAME,
    PA_COMMAND_LOOKUP_SINK,
    PA_COMMAND_LOOKUP_SOURCE,
    PA_COMMAND_DRAIN_PLAYBACK_STREAM,
    PA_COMMAND_PLAYBACK_STREAM_KILLED,
    PA_COMMAND_RECORD_STREAM_KILLED,
    PA_COMMAND_STAT,
    PA_COMMAND_GET_PLAYBACK_LATENCY,
    PA_COMMAND_MAX
};

enum {
    PA_ERROR_OK,
    PA_ERROR_ACCESS,
    PA_ERROR_COMMAND,
    PA_ERROR_INVALID,
    PA_ERROR_EXIST,
    PA_ERROR_NOENTITY,
    PA_ERROR_CONNECTIONREFUSED,
    PA_ERROR_PROTOCOL,
    PA_ERROR_TIMEOUT,
    PA_ERROR_AUTHKEY,
    PA_ERROR_INTERNAL,
    PA_ERROR_CONNECTIONTERMINATED,
    PA_ERROR_KILLED,
    PA_ERROR_INVALIDSERVER,
    PA_ERROR_MAX
};

#define PA_NATIVE_COOKIE_LENGTH 256
#define PA_NATIVE_COOKIE_FILE ".polypaudio-cookie"

#endif
