// Licensed to the .NET Foundation under one or more agreements.
// The .NET Foundation licenses this file to you under the MIT license.

// Diagnostics IPC PAL stub for libnx: no Unix domain sockets or TCP loopback
// on Horizon. Everything fails inert; the diagnostic server just never gets
// a listener.

#include "ds-rt-config.h"

#ifdef ENABLE_PERFTRACING

#include "ds-ipc-pal.h"

bool
ds_ipc_pal_init (void)
{
	return true;
}

bool
ds_ipc_pal_shutdown (void)
{
	return true;
}

int32_t
ds_ipc_get_handle_int32_t (DiagnosticsIpc *ipc)
{
	return -1;
}

DiagnosticsIpc *
ds_ipc_alloc (
	const ep_char8_t *ipc_name,
	DiagnosticsIpcConnectionMode mode,
	ds_ipc_error_callback_func callback)
{
	return NULL;
}

void
ds_ipc_free (DiagnosticsIpc *ipc)
{
}

void
ds_ipc_reset (DiagnosticsIpc *ipc)
{
}

int32_t
ds_ipc_poll (
	DiagnosticsIpcPollHandle *poll_handles_data,
	size_t poll_handles_data_len,
	uint32_t timeout_ms,
	ds_ipc_error_callback_func callback)
{
	return -1;
}

bool
ds_ipc_listen (
	DiagnosticsIpc *ipc,
	ds_ipc_error_callback_func callback)
{
	return false;
}

DiagnosticsIpcStream *
ds_ipc_accept (
	DiagnosticsIpc *ipc,
	ds_ipc_error_callback_func callback)
{
	return NULL;
}

DiagnosticsIpcStream *
ds_ipc_connect (
	DiagnosticsIpc *ipc,
	uint32_t timeout_ms,
	ds_ipc_error_callback_func callback,
	bool *timed_out)
{
	if (timed_out)
		*timed_out = false;
	return NULL;
}

void
ds_ipc_close (
	DiagnosticsIpc *ipc,
	bool is_shutdown,
	ds_ipc_error_callback_func callback)
{
}

int32_t
ds_ipc_to_string (
	DiagnosticsIpc *ipc,
	ep_char8_t *buffer,
	uint32_t buffer_len)
{
	return 0;
}

int32_t
ds_ipc_stream_get_handle_int32_t (DiagnosticsIpcStream *ipc_stream)
{
	return -1;
}

IpcStream *
ds_ipc_stream_get_stream_ref (DiagnosticsIpcStream *ipc_stream)
{
	return NULL;
}

void
ds_ipc_stream_free (DiagnosticsIpcStream *ipc_stream)
{
}

bool
ds_ipc_stream_read (
	DiagnosticsIpcStream *ipc_stream,
	uint8_t *buffer,
	uint32_t bytes_to_read,
	uint32_t *bytes_read,
	uint32_t timeout_ms)
{
	return false;
}

bool
ds_ipc_stream_read_fd (
	DiagnosticsIpcStream *ipc_stream,
	int *data_fd)
{
	return false;
}

bool
ds_ipc_stream_write (
	DiagnosticsIpcStream *ipc_stream,
	const uint8_t *buffer,
	uint32_t bytes_to_write,
	uint32_t *bytes_written,
	uint32_t timeout_ms)
{
	return false;
}

bool
ds_ipc_stream_flush (DiagnosticsIpcStream *ipc_stream)
{
	return false;
}

bool
ds_ipc_stream_close (
	DiagnosticsIpcStream *ipc_stream,
	ds_ipc_error_callback_func callback)
{
	return false;
}

int32_t
ds_ipc_stream_to_string (
	DiagnosticsIpcStream *ipc_stream,
	ep_char8_t *buffer,
	uint32_t buffer_len)
{
	return 0;
}

IpcPollEvents
ds_ipc_stream_poll (
	DiagnosticsIpcStream *ipc_stream,
	uint32_t timeout_ms)
{
	return (IpcPollEvents)0;
}

#endif /* ENABLE_PERFTRACING */
