/*
 * $Id$
 */

/* py-sendfile

   A Python module interface to sendfile(2)
   Copyright (C) 2005 Ben Woolley <user tautolog at gmail>

   The AIX support code is:

   Copyright (C) 2008,2009 Niklas Edmundsson <nikke@acc.umu.se>

   Currently maintained by Giampaolo Rodola'
   Copyright (C) 2011 <g.rodola@gmail.com>

   This is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public
   License as published by the Free Software Foundation; either
   version 2.1 of the License, or (at your option) any later version.

   This is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with the GNU C Library; if not, write to the Free
   Software Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA
   02111-1307 USA.  */

#include <Python.h>
#include <stdlib.h>


/* --- begin FreeBSD / Dragonfly --- */
#if defined(__FreeBSD__) || defined(__DragonFly__)
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/uio.h>

int
PyParse_off_t(PyObject* arg, void* addr)
{
#if !defined(HAVE_LARGEFILE_SUPPORT)
    *((off_t*)addr) = PyLong_AsLong(arg);
#else
    *((off_t*)addr) = PyLong_Check(arg) ? PyLong_AsLongLong(arg)
 : PyLong_AsLong(arg);
#endif
    if (PyErr_Occurred())
        return 0;
    return 1;
}


static int
iov_setup(struct iovec **iov, Py_buffer **buf, PyObject *seq, int cnt, int type)
{
    int i, j;
    *iov = PyMem_New(struct iovec, cnt);
    if (*iov == NULL) {
        PyErr_NoMemory();
        return 0;
    }
    *buf = PyMem_New(Py_buffer, cnt);
    if (*buf == NULL) {
        PyMem_Del(*iov);
        PyErr_NoMemory();
        return 0;
    }

    for (i = 0; i < cnt; i++) {
        if (PyObject_GetBuffer(PySequence_GetItem(seq, i), &(*buf)[i],
                type) == -1) {
            PyMem_Del(*iov);
            for (j = 0; j < i; j++) {
                PyBuffer_Release(&(*buf)[j]);
           }
            PyMem_Del(*buf);
            return 0;
        }
        (*iov)[i].iov_base = (*buf)[i].buf;
        (*iov)[i].iov_len = (*buf)[i].len;
    }
    return 1;
}

static void
iov_cleanup(struct iovec *iov, Py_buffer *buf, int cnt)
{
    int i;
    PyMem_Del(iov);
    for (i = 0; i < cnt; i++) {
        PyBuffer_Release(&buf[i]);
    }
    PyMem_Del(buf);
}

static PyObject *
method_sendfile(PyObject *self, PyObject *args, PyObject *kwdict)
{
    int in, out;
    Py_ssize_t ret;
    Py_ssize_t len;
    off_t offset;
    PyObject *headers = NULL, *trailers = NULL;
    Py_buffer *hbuf, *tbuf;
    off_t sbytes;
    struct sf_hdtr sf;
    int flags = 0;
    sf.headers = NULL;
    sf.trailers = NULL;

    static char *keywords[] = {"out", "in", "offset", "count",
                               "headers", "trailers", "flags", NULL};

    if (!PyArg_ParseTupleAndKeywords(args, kwdict, "iiO&n|OOi:sendfile",
        keywords, &out, &in, PyParse_off_t, &offset, &len,
        &headers, &trailers, &flags)) {
            return NULL;
    }

    if (headers != NULL) {
        if (!PySequence_Check(headers)) {
            PyErr_SetString(PyExc_TypeError,
                "sendfile() headers must be a sequence or None");
            return NULL;
        } else {
            sf.hdr_cnt = PySequence_Size(headers);
            if (sf.hdr_cnt > 0 && !iov_setup(&(sf.headers), &hbuf,
                    headers, sf.hdr_cnt, PyBUF_SIMPLE))
                return NULL;
        }
    }
    if (trailers != NULL) {
        if (!PySequence_Check(trailers)) {
            PyErr_SetString(PyExc_TypeError,
                "sendfile() trailers must be a sequence or None");
            return NULL;
        } else {
            sf.trl_cnt = PySequence_Size(trailers);
            if (sf.trl_cnt > 0 && !iov_setup(&(sf.trailers), &tbuf,
                    trailers, sf.trl_cnt, PyBUF_SIMPLE))
                return NULL;
        }
    }

    Py_BEGIN_ALLOW_THREADS
    ret = sendfile(in, out, offset, len, &sf, &sbytes, flags);
    Py_END_ALLOW_THREADS

    if (sf.headers != NULL)
        iov_cleanup(sf.headers, hbuf, sf.hdr_cnt);
    if (sf.trailers != NULL)
         iov_cleanup(sf.trailers, tbuf, sf.trl_cnt);

    if (ret < 0) {
        if ((errno == EAGAIN) || (errno == EBUSY)) {
            if (sbytes != 0) {
                goto done;
            }
            else {
                // upper application is supposed to retry
                PyErr_SetFromErrno(PyExc_OSError);
                return NULL;
            }
        }
        PyErr_SetFromErrno(PyExc_OSError);
        return NULL;
    }

    goto done;

done:
    #if !defined(HAVE_LARGEFILE_SUPPORT)
        return Py_BuildValue("ll", sbytes, offset + sbytes);
    #else
        return Py_BuildValue("LL", sbytes, offset + sbytes);
    #endif
}
/* --- end FreeBSD / Dragonfly --- */

/* --- begin AIX --- */
#elif defined(_AIX)
#include <sys/socket.h>

static PyObject *
method_sendfile(PyObject *self, PyObject *args)
{
    int out_fd, in_fd;
    off_t offset;
    size_t count;
    char *hdr=NULL, *trail=NULL;
    int hdrsize, trailsize;
    ssize_t sts=0;
    struct sf_parms sf_iobuf;
    int rc;

    if (!PyArg_ParseTuple(args, "iiLk|s#s#",
                          &out_fd, &in_fd, &offset, &count, &hdr, &hdrsize,
			  &trail, &trailsize))
        return NULL;

    if(hdr != NULL) {
        sf_iobuf.header_data = hdr;
        sf_iobuf.header_length = hdrsize;
    }
    else {
        sf_iobuf.header_data = NULL;
        sf_iobuf.header_length = 0;
    }
    if(trail != NULL) {
        sf_iobuf.trailer_data = trail;
        sf_iobuf.trailer_length = trailsize;
    }
    else {
	sf_iobuf.trailer_data = NULL;
	sf_iobuf.trailer_length = 0;
    }
    sf_iobuf.file_descriptor = in_fd;
    sf_iobuf.file_offset = offset;
    sf_iobuf.file_bytes = count;

    Py_BEGIN_ALLOW_THREADS;
    do {
	sf_iobuf.bytes_sent = 0; /* Really needed? */
        rc = send_file(&out_fd, &sf_iobuf, SF_DONT_CACHE);
	sts += sf_iobuf.bytes_sent;
    } while( rc == 1 || (rc == -1 && errno == EINTR) );
    Py_END_ALLOW_THREADS;

    offset = sf_iobuf.file_offset;

    if (rc == -1) {
        PyErr_SetFromErrno(PyExc_OSError);
        return NULL;
    } else {
        return Py_BuildValue("kL", sts, offset);
    }
}
/* --- end AIX --- */

/* --- start Linux --- */

#elif defined (__linux__)
#include <sys/sendfile.h>

static PyObject *
method_sendfile(PyObject *self, PyObject *args)
{
    int out_fd, in_fd;
    off_t offset;
    size_t count;
    ssize_t sts;

    if (!PyArg_ParseTuple(args, "iiLk", &out_fd, &in_fd, &offset, &count))
        return NULL;

    Py_BEGIN_ALLOW_THREADS;
    sts = sendfile(out_fd, in_fd, &offset, count);
    Py_END_ALLOW_THREADS;
    if (sts == -1) {
        PyErr_SetFromErrno(PyExc_OSError);
        return NULL;
    }
#if !defined(HAVE_LARGEFILE_SUPPORT)
    return Py_BuildValue("lL", sts, offset);
#else
    return Py_BuildValue("LL", sts, offset);
#endif
}

#endif  /* --- end Linux --- */


static PyMethodDef
SendfileMethods[] =
{
    {"sendfile",  method_sendfile,  METH_VARARGS | METH_KEYWORDS,
"sendfile(out, in, offset, nbytes)\n"
"sendfile(out, in, offset, nbytes, headers=None, trailers=None, flags=0)\n"
"\n"
"Copy *nbytes* bytes from file descriptor *in* to file descriptor *out*.\n"
"\n"
"The first case is supported by all platforms.\n"
"Return value is a tuple, (byteswritten, offset), where offset is a value\n"
"pointing to the byte following the last byte read.\n"
"\n"
"On Linux, if *offset* is given as `None`, the bytes are read from the\n"
"current position of *in* and the position of *in* is updated. It returns\n"
"the same as above with offset being `None`.\n"
"\n"
"The second case may be used on Mac OS X and FreeBSD where *headers* and\n"
"*trailers* are arbitrary sequences of buffers that are written before and\n"
"after the data from *in* is written. It returns the same as the first case.\n"
"\n"
"On Mac OS X and FreeBSD, a value of 0 for *nbytes* specifies to send until\n"
"the end of *in* is reached.\n"
"\n"
"On Solaris, *out* may be the file descriptor of a regular file or the file\n"
"descriptor of a socket. On all other platforms, *out* must be the file\n"
"descriptor of an open socket.\n"
"\n"
    },
    {NULL, NULL, 0, NULL}        /* Sentinel */
};

struct module_state {
    PyObject *error;
};

#if PY_MAJOR_VERSION >= 3
#define GETSTATE(m) ((struct module_state*)PyModule_GetState(m))
#else
#define GETSTATE(m) (&_state)
static struct module_state _state;
#endif

#if PY_MAJOR_VERSION >= 3
static int
sendfile_traverse(PyObject *m, visitproc visit, void *arg) {
    Py_VISIT(GETSTATE(m)->error);
    return 0;
}

static int
sendfile_clear(PyObject *m) {
    Py_CLEAR(GETSTATE(m)->error);
    return 0;
}

static struct PyModuleDef
moduledef = {
        PyModuleDef_HEAD_INIT,
        "sendfile",
        NULL,
        sizeof(struct module_state),
        SendfileMethods,
        NULL,
        sendfile_traverse,
        sendfile_clear,
        NULL
};

#define INITERROR return NULL

PyObject *
PyInit_sendfile(void)

#else
#define INITERROR return

void initsendfile(void)
#endif
{
#if PY_MAJOR_VERSION >= 3
    PyObject *module = PyModule_Create(&moduledef);
#else
    PyObject *module = Py_InitModule("sendfile", SendfileMethods);
#endif

    // constants
#ifdef SF_NODISKIO
    PyModule_AddIntConstant(module, "SF_NODISKIO", SF_NODISKIO);
#endif
#ifdef SF_MNOWAIT
    PyModule_AddIntConstant(module, "SF_MNOWAIT", SF_MNOWAIT);
#endif
#ifdef SF_SYNC
    PyModule_AddIntConstant(module, "SF_SYNC", SF_SYNC);
#endif

    if (module == NULL) {
        INITERROR;
    }
#if PY_MAJOR_VERSION >= 3
    return module;
#endif
}

