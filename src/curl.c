/* $Id: curl.c,v 1.166 2002/10/24 20:10:05 mfx Exp $ */

/* PycURL -- cURL Python module
 *
 * Authors:
 *  Kjetil Jacobsen <kjetilja at cs.uit.no>
 *  Markus F.X.J. Oberhumer <markus at oberhumer.com>
 *
 * Contributions:
 *  Tino Lange <Tino.Lange at gmx.de>
 *  Matt King <matt at gnik.com>
 *  Conrad Steenberg <conrad at hep.caltech.edu>
 *  Amit Mongia <amit_mongia at hotmail.com>
 *  Eric S. Raymond <esr at thyrsus.com>
 *
 * See file COPYING for license information.
 *
 */

#include <sys/types.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#undef NDEBUG
#include <assert.h>

#if (defined(_WIN32) || defined(__WIN32__)) && !defined(WIN32)
#  define WIN32 1
#endif
#include <Python.h>
#include <curl/curl.h>
#include <curl/multi.h>

/* Ensure we have updated versions */
#if !defined(PY_VERSION_HEX) || (PY_VERSION_HEX < 0x010502f0)
#  error "Need Python version 1.5.2 or greater to compile pycurl."
#endif
#if !defined(LIBCURL_VERSION_NUM) || (LIBCURL_VERSION_NUM < 0x070a01)
#  error "Need libcurl version 7.10.1 or greater to compile pycurl."
#endif

/* Beginning with Python 2.2 we support Cyclic Garbarge Collection */
#undef USE_GC
#if 1 && (PY_VERSION_HEX >= 0x02020000)
#  define USE_GC
#endif


static PyObject *ErrorObject;

typedef struct {
    PyObject_HEAD
    PyObject *dict;                 /* Python attributes dictionary */
    CURLM *multi_handle;
    PyThreadState *state;
    fd_set read_fd_set;
    fd_set write_fd_set;
    fd_set exc_fd_set;
} CurlMultiObject;

typedef struct {
    PyObject_HEAD
    PyObject *dict;                 /* Python attributes dictionary */
    CURL *handle;
    PyThreadState *state;
    CurlMultiObject *multi_stack;   /* refcounted Python object */
    struct curl_httppost *httppost;
    struct curl_slist *httpheader;
    struct curl_slist *quote;
    struct curl_slist *postquote;
    struct curl_slist *prequote;
    PyObject *w_cb;
    PyObject *h_cb;
    PyObject *r_cb;
    PyObject *pro_cb;
    PyObject *pwd_cb;
    PyObject *d_cb;
    PyObject *readdata;
    PyObject *writedata;
    PyObject *writeheader;
    int writeheader_set;
    char error[CURL_ERROR_SIZE+1];
    void *options[CURLOPT_LASTENTRY];
} CurlObject;

#if !defined(__cplusplus)
staticforward PyTypeObject Curl_Type;
staticforward PyTypeObject CurlMulti_Type;
#endif

#define CURLERROR() do {\
    PyObject *v; \
    v = Py_BuildValue("(is)", (int) (res), self->error); \
    PyErr_SetObject(ErrorObject, v); \
    Py_DECREF(v); \
    return NULL; \
} while (0)

#define CURLERROR2(msg) do {\
    PyObject *v; \
    v = Py_BuildValue("(is)", (int) (res), msg); \
    PyErr_SetObject(ErrorObject, v); \
    Py_DECREF(v); \
    return NULL; \
} while (0)


#undef UNUSED
#define UNUSED(var)     ((void)&var)


/*************************************************************************
// static utility functions
**************************************************************************/

static PyThreadState *
get_thread_state(const CurlObject *self)
{
    /* Get the thread state for callbacks to run in.
     * This is either `self->state' when running inside perform() or
     * `self->multi_stack->state' when running inside multi_perform().
     * When the result is != NULL we also implicitly assert
     * a valid `self->handle'.
     */
    if (self == NULL)
        return NULL;
    if (self->state != NULL)
    {
        /* inside perform() */
        assert(self->handle != NULL);
        if (self->multi_stack != NULL) {
            assert(self->multi_stack->state == NULL);
        }
        return self->state;
    }
    if (self->multi_stack != NULL && self->multi_stack->state != NULL)
    {
        /* inside multi_perform() */
        assert(self->handle != NULL);
        assert(self->multi_stack->multi_handle != NULL);
        assert(self->state == NULL);
        return self->multi_stack->state;
    }
    return NULL;
}


/* assert some CurlObject invariants */
static void
assert_curl_object(const CurlObject *self)
{
    assert(self != NULL);
    assert(self->ob_type == &Curl_Type);
    (void) get_thread_state(self);
}


/* assert some CurlMultiObject invariants */
static void
assert_curl_multi_object(const CurlMultiObject *self)
{
    assert(self != NULL);
    assert(self->ob_type == &CurlMulti_Type);
    if (self->state != NULL) {
        assert(self->multi_handle != NULL);
    }
}


/*************************************************************************
// CurlObject
**************************************************************************/

/* --------------- construct/destruct (i.e. open/close) --------------- */

/* Allocate a new python curl object */
static CurlObject *
util_curl_new(void)
{
    CurlObject *self;

#if defined(USE_GC)
    self = (CurlObject *) PyObject_GC_New(CurlObject, &Curl_Type);
    if (self) PyObject_GC_Track(self);
#elif (PY_VERSION_HEX >= 0x01060000)
    self = (CurlObject *) PyObject_New(CurlObject, &Curl_Type);
#else
    self = (CurlObject *) PyObject_NEW(CurlObject, &Curl_Type);
#endif
    if (self == NULL)
        return NULL;

    /* Set python curl object initial values */
    self->dict = NULL;
    self->handle = NULL;
    self->state = NULL;
    self->multi_stack = NULL;
    self->httppost = NULL;
    self->httpheader = NULL;
    self->quote = NULL;
    self->postquote = NULL;
    self->prequote = NULL;

    /* Set callback pointers to NULL by default */
    self->w_cb = NULL;
    self->h_cb = NULL;
    self->r_cb = NULL;
    self->pro_cb = NULL;
    self->pwd_cb = NULL;
    self->d_cb = NULL;

    /* Set file object pointers to NULL by default */
    self->readdata = NULL;
    self->writedata = NULL;
    self->writeheader = NULL;
    self->writeheader_set = 0;

    /* Zero string pointer memory buffer used by setopt */
    memset(self->options, 0, sizeof(self->options));
    memset(self->error, 0, sizeof(self->error));

    return self;
}


/* constructor - this is a module-level function returning a new instance */
static CurlObject *
do_curl_new(PyObject *dummy, PyObject *args)
{
    CurlObject *self;
    int res;

    UNUSED(dummy);
    if (!PyArg_ParseTuple(args, ":Curl")) {
        return NULL;
    }

    /* Allocate python curl object */
    self = util_curl_new();
    if (self == NULL)
        return NULL;

    /* Initialize curl handle */
    self->handle = curl_easy_init();
    if (self->handle == NULL)
        goto error;

    /* Set curl error buffer and zero it */
    res = curl_easy_setopt(self->handle, CURLOPT_ERRORBUFFER, self->error);
    if (res != CURLE_OK)
        goto error;
    memset(self->error, 0, sizeof(self->error));

    /* Enable NOPROGRESS by default, i.e. no progress output */
    res = curl_easy_setopt(self->handle, CURLOPT_NOPROGRESS, 1);
    if (res != CURLE_OK)
        goto error;

    /* Disable VERBOSE by default, i.e. no verbose output */
    res = curl_easy_setopt(self->handle, CURLOPT_VERBOSE, 0);
    if (res != CURLE_OK)
        goto error;

    /* Success - return new object */
    return self;

error:
    Py_DECREF(self);    /* this also closes self->handle */
    PyErr_SetString(ErrorObject, "initializing curl failed");
    return NULL;
}


#if 0 /* Curl.copy() needs some more work */
static CurlObject *
do_curl_copy(const CurlObject *self, PyObject *args)
{
    CurlObject *copy = NULL;
    int res;
    int i;

    /* Sanity checks */
    if (!PyArg_ParseTuple(args, ":copy")) {
        return NULL;
    }
    if (self->handle == NULL) {
        PyErr_SetString(ErrorObject, "cannot invoke copy, no curl handle");
        return NULL;
    }
    if (get_thread_state(self) != NULL) {
        PyErr_SetString(ErrorObject, "cannot invoke copy - already running");
        return NULL;
    }

    /* FIXME FIXME FIXME
    **   - we really need some sane semantics
    **   - have to carefully check what curl_easy_duphandle() actually does
    **
    ** refuse to copy ?
    **   - when we have open file objects
    ** deep copy ?
    **   - dict
    ** shallow copy ?
    **   - callbacks
    ** NOT copy ?
    **   - multi_stack
    **/

#if 1
    /* for now we refuse to copy if *ANY* value except multi_stack is set */
    /* this makes copy() pretty useless */
    if (self->dict)
        goto cannot_copy;
    if (self->httppost || self->httpheader || self->quote || self->postquote ||  self->prequote)
        goto cannot_copy;
    if (self->w_cb || self->r_cb || self->pro_cb || self->pwd_cb || self->d_cb)
        goto cannot_copy;
    if (self->readdata || self->writedata || self->writeheader || self->writeheader_set)
        goto cannot_copy;
    for (i = 0; i < CURLOPT_LASTENTRY; i++) {
        if (self->options[i] != NULL) {
            goto cannot_copy;
        }
    }
#endif

    /* Allocate python curl object */
    copy = util_curl_new();
    if (copy == NULL)
        return NULL;

    /* Copy curl handle */
    copy->handle = curl_easy_duphandle(self->handle);
    if (copy->handle == NULL)
        goto error;

    /* Set curl error buffer and copy it */
    res = curl_easy_setopt(copy->handle, CURLOPT_ERRORBUFFER, copy->error);
    if (res != CURLE_OK)
        goto error;
    memcpy(copy->error, self->error, sizeof(self->error));

    /* Success - return new object */
    return copy;

error:
    Py_XDECREF(copy);    /* this also closes copy->handle */
    PyErr_SetString(ErrorObject, "initializing curl failed");
    return NULL;

cannot_copy:
    Py_XDECREF(copy);    /* this also closes copy->handle */
    PyErr_SetString(ErrorObject, "Curl object is in a state that cannot get copied");
    return NULL;
}
#endif /* #if 0 */


/* util function shared by close() and clear() */
static void
util_curl_xdecref(CurlObject *self, int flags, CURL *handle)
{
#undef XDECREF
#define XDECREF(v)  Py_XDECREF(v); v = NULL

    if (flags & 1) {
        /* Decrement refcount for attributes dictionary. */
        XDECREF(self->dict);
    }

    if (flags & 2) {
        /* Decrement refcount for multi_stack. */
        if (self->multi_stack != NULL) {
            CurlMultiObject *multi_stack = self->multi_stack;
            self->multi_stack = NULL;
            if (multi_stack->multi_handle != NULL && handle != NULL) {
                (void) curl_multi_remove_handle(multi_stack->multi_handle, handle);
            }
            Py_DECREF(multi_stack);
        }
    }

    if (flags & 4) {
        /* Decrement refcount for python callbacks. */
        XDECREF(self->w_cb);
        XDECREF(self->r_cb);
        XDECREF(self->pro_cb);
        XDECREF(self->pwd_cb);
        XDECREF(self->h_cb);
        XDECREF(self->d_cb);
    }

    if (flags & 8) {
        /* Decrement refcount for python file objects. */
        XDECREF(self->readdata);
        XDECREF(self->writedata);
        XDECREF(self->writeheader);
        self->writeheader_set = 0;
    }
#undef XDECREF
}


static void
util_curl_close(CurlObject *self)
{
    CURL *handle;
    int i;

    /* Zero handle and thread-state to disallow any operations to be run
     * from now on */
    assert(self != NULL);
    assert(self->ob_type == &Curl_Type);
    handle = self->handle;
    self->handle = NULL;
    if (handle == NULL) {
        /* Some paranoia assertions just to make sure the object
         * deallocation problem is finally really fixed... */
        assert(self->state == NULL);
        assert(self->multi_stack == NULL);
        return;             /* already closed */
    }
    self->state = NULL;

    util_curl_xdecref(self, 2 | 4 | 8, handle);

    /* Free all variables allocated by setopt */
#undef SFREE
#define SFREE(v)   if (v != NULL) (curl_formfree(v), v = NULL)
    SFREE(self->httppost);
#undef SFREE
#define SFREE(v)   if (v != NULL) (curl_slist_free_all(v), v = NULL)
    SFREE(self->httpheader);
    SFREE(self->quote);
    SFREE(self->postquote);
    SFREE(self->prequote);
#undef SFREE
    for (i = 0; i < CURLOPT_LASTENTRY; i++) {
        if (self->options[i] != NULL) {
            free(self->options[i]);
            self->options[i] = NULL;
        }
    }

    /* Finally free the curl handle */
    if (handle != NULL) {
        /* Must be done without the gil */
        Py_BEGIN_ALLOW_THREADS
        curl_easy_cleanup(handle);
        Py_END_ALLOW_THREADS
    }
}


static void
do_curl_dealloc(CurlObject *self)
{
#if defined(USE_GC)
    PyObject_GC_UnTrack(self);
    Py_TRASHCAN_SAFE_BEGIN(self)
#endif

    Py_XDECREF(self->dict);
    self->dict = NULL;
    util_curl_close(self);

#if defined(USE_GC)
    PyObject_GC_Del(self);
#elif (PY_VERSION_HEX >= 0x01060000)
    PyObject_Del(self);
#else
    PyMem_DEL(self);
#endif

#if defined(USE_GC)
    Py_TRASHCAN_SAFE_END(self)
#endif
}


static PyObject *
do_curl_close(CurlObject *self, PyObject *args)
{
    if (!PyArg_ParseTuple(args, ":close")) {
        return NULL;
    }
    if (get_thread_state(self) != NULL) {
        PyErr_SetString(ErrorObject, "cannot invoke close, perform() is running");
        return NULL;
    }
    util_curl_close(self);
    Py_INCREF(Py_None);
    return Py_None;
}


/* --------------- GC support --------------- */

#if defined(USE_GC)

/* Drop references that may have created reference cycles. */
static int
do_curl_clear(CurlObject *self)
{
    assert(get_thread_state(self) == NULL);
    util_curl_xdecref(self, 1 | 2 | 4 | 8, self->handle);
    return 0;
}

/* Traverse all refcounted objects. */
static int
do_curl_traverse(CurlObject *self, visitproc visit, void *arg)
{
    int err;
#undef VISIT
#define VISIT(v)    if ((v) != NULL && ((err = visit(v, arg)) != 0)) return err

    VISIT(self->dict);
    VISIT((PyObject *) self->multi_stack);

    VISIT(self->w_cb);
    VISIT(self->r_cb);
    VISIT(self->pro_cb);
    VISIT(self->pwd_cb);
    VISIT(self->h_cb);
    VISIT(self->d_cb);

    VISIT(self->readdata);
    VISIT(self->writedata);
    VISIT(self->writeheader);

    return 0;

#undef VISIT
}

#endif /* USE_GC */


/* --------------- perform --------------- */

static PyObject *
do_curl_perform(CurlObject *self, PyObject *args)
{
    int res;

    /* Sanity checks */
    if (!PyArg_ParseTuple(args, ":perform")) {
        return NULL;
    }

    if (self->handle == NULL) {
        PyErr_SetString(ErrorObject, "cannot invoke perform, no curl handle");
        return NULL;
    }
    if (get_thread_state(self) != NULL) {
        PyErr_SetString(ErrorObject, "cannot invoke perform - already running");
        return NULL;
    }

    /* Save handle to current thread (used as context for python callbacks) */
    self->state = PyThreadState_Get();
    assert(self->state != NULL);

    /* Release global lock and start */
    Py_BEGIN_ALLOW_THREADS
    res = curl_easy_perform(self->handle);
    Py_END_ALLOW_THREADS

    /* Zero thread-state to disallow callbacks to be run from now on */
    self->state = NULL;

    if (res != CURLE_OK) {
        CURLERROR();
    }
    Py_INCREF(Py_None);
    return Py_None;
}


/* --------------- callback handlers --------------- */

static size_t
util_write_callback(int flags, char *ptr, size_t size, size_t nmemb, void *stream)
{
    PyObject *arglist;
    PyObject *result;
    PyObject *cb;
    CurlObject *self;
    PyThreadState *tmp_state;
    int write_size;
    size_t ret = 0;     /* assume error */

    self = (CurlObject *)stream;
    tmp_state = get_thread_state(self);
    if (tmp_state == NULL) {
        return ret;
    }
    cb = flags ? self->h_cb : self->w_cb;
    if (cb == NULL) {
        return ret;
    }
    write_size = (int)(size * nmemb);
    if (write_size <= 0) {
        return ret;
    }

    PyEval_AcquireThread(tmp_state);
    arglist = Py_BuildValue("(s#)", ptr, write_size);
    result = PyEval_CallObject(cb, arglist);
    Py_DECREF(arglist);
    if (result == NULL) {
        PyErr_Print();
    }
    else if (result == Py_None) {               /* None means success */
        ret = (size_t)write_size;
    }
    else {
        write_size = (int)PyInt_AsLong(result);
        if (write_size >= 0)
            ret = (size_t)write_size;                   /* success */
    }
    Py_XDECREF(result);
    PyEval_ReleaseThread(tmp_state);
    return ret;
}


static size_t
write_callback(char *ptr, size_t size, size_t nmemb, void *stream)
{
    return util_write_callback(0, ptr, size, nmemb, stream);
}

static size_t
header_callback(char *ptr, size_t size, size_t nmemb, void *stream)
{
    return util_write_callback(1, ptr, size, nmemb, stream);
}


static size_t
read_callback(char *ptr, size_t size, size_t nmemb, void  *stream)
{
    PyObject *arglist;
    PyObject *result;
    CurlObject *self;
    PyThreadState *tmp_state;
    char *buf;
    int obj_size, read_size;
    size_t ret = 0;     /* assume error */

    self = (CurlObject *)stream;
    tmp_state = get_thread_state(self);
    if (tmp_state == NULL || self->r_cb == NULL) {
        return ret;
    }
    read_size = (int)(size * nmemb);
    if (read_size <= 0) {
        return ret;
    }

    PyEval_AcquireThread(tmp_state);
    arglist = Py_BuildValue("(i)", read_size);
    result = PyEval_CallObject(self->r_cb, arglist);
    Py_DECREF(arglist);
    if (result == NULL) {
        PyErr_Print();
    }
    else {
        if (!PyString_Check(result)) {
            PyErr_SetString(ErrorObject, "callback for READFUNCTION must return string");
            PyErr_Print();
        }
        else {
#if (PY_VERSION_HEX >= 0x02000000)
            PyString_AsStringAndSize(result, &buf, &obj_size);
#else
            buf = PyString_AS_STRING(result);
            obj_size = PyString_GET_SIZE(result);
#endif
            if (obj_size > read_size) {
                PyErr_SetString(ErrorObject, "string from READFUNCTION callback is too long");
                PyErr_Print();
            }
            else {
                memcpy(ptr, buf, obj_size);
                ret = (size_t)obj_size;         /* success */
            }
        }
    }
    Py_XDECREF(result);
    PyEval_ReleaseThread(tmp_state);
    return ret;
}


static int
progress_callback(void *client,
                  double dltotal,
                  double dlnow,
                  double ultotal,
                  double ulnow)
{
    PyObject *arglist;
    PyObject *result;
    CurlObject *self;
    PyThreadState *tmp_state;
    int ret = 1;       /* assume error */

    self = (CurlObject *)client;
    tmp_state = get_thread_state(self);
    if (tmp_state == NULL || self->pro_cb == NULL) {
        return ret;
    }

    PyEval_AcquireThread(tmp_state);
    arglist = Py_BuildValue("(dddd)", dltotal, dlnow, ultotal, ulnow);
    result = PyEval_CallObject(self->pro_cb, arglist);
    Py_DECREF(arglist);
    if (result == NULL) {
        PyErr_Print();
    }
    else if (result == Py_None) {               /* None means success */
        ret = 0;
    }
    else {
        ret = (int)PyInt_AsLong(result);
    }
    Py_XDECREF(result);
    PyEval_ReleaseThread(tmp_state);
    return ret;
}


static int
password_callback(void *client, const char *prompt, char* buffer, int buflen)
{
    PyObject *arglist;
    PyObject *result;
    CurlObject *self;
    PyThreadState *tmp_state;
    char *buf;
    int ret = 1;       /* assume error */

    self = (CurlObject *)client;
    tmp_state = get_thread_state(self);
    if (tmp_state == NULL || self->pwd_cb == NULL) {
        return ret;
    }

    PyEval_AcquireThread(tmp_state);
    arglist = Py_BuildValue("(si)", prompt, buflen);
    result = PyEval_CallObject(self->pwd_cb, arglist);
    Py_DECREF(arglist);
    if (result == NULL) {
        PyErr_Print();
    }
    else {
        if (!PyString_Check(result)) {
            PyErr_SetString(ErrorObject, "callback for PASSWDFUNCTION must return string");
            PyErr_Print();
        }
        else {
            buf = PyString_AsString(result);
            if ((int)strlen(buf) > buflen) {
                PyErr_SetString(ErrorObject, "string from PASSWDFUNCTION callback is too long");
                PyErr_Print();
            }
            else {
                strcpy(buffer, buf);
                ret = 0;        /* success */
            }
        }
    }
    Py_XDECREF(result);
    PyEval_ReleaseThread(tmp_state);
    return ret;
}


static int
debug_callback(CURL *curlobj,
               curl_infotype type,
               char *buffer,
               size_t size,
               void *data)
{
    PyObject *arglist;
    PyObject *result;
    CurlObject *self;
    PyThreadState *tmp_state;
    int ret = 0;       /* always success */

    UNUSED(curlobj);
    self = (CurlObject *)data;
    tmp_state = get_thread_state(self);
    if (tmp_state == NULL || self->d_cb == NULL) {
        return ret;
    }
    if ((int)size < 0) {
        return ret;
    }

    PyEval_AcquireThread(tmp_state);
    arglist = Py_BuildValue("(is#)", (int)type, buffer, (int)size);
    result = PyEval_CallObject(self->d_cb, arglist);
    Py_DECREF(arglist);
    if (result == NULL) {
        PyErr_Print();
    }
    PyEval_ReleaseThread(tmp_state);
    return ret;
}


/* --------------- setopt/getinfo --------------- */

static PyObject *
do_curl_setopt(CurlObject *self, PyObject *args)
{
    int option, opt_masked;
    char *stringdata;
    long longdata;
    char *buf;
    PyObject *obj;
    FILE *fp;
    int res = -1;
    int len;
    char *str;
    int i;

    /* Check that we have a valid curl handle for the object */
    if (self->handle == NULL) {
        PyErr_SetString(ErrorObject, "cannot invoke setopt, no curl handle");
        return NULL;
    }
    if (get_thread_state(self) != NULL) {
        PyErr_SetString(ErrorObject, "cannot invoke setopt, perform() is running");
        return NULL;
    }

    /* Handle the case of string arguments */
    if (PyArg_ParseTuple(args, "is:setopt", &option, &stringdata)) {
        /* Check that the option specified a string as well as the input */
        if (!(option == CURLOPT_URL ||
              option == CURLOPT_PROXY ||
              option == CURLOPT_USERPWD ||
              option == CURLOPT_PROXYUSERPWD ||
              option == CURLOPT_RANGE ||
              option == CURLOPT_POSTFIELDS ||
              option == CURLOPT_REFERER ||
              option == CURLOPT_USERAGENT ||
              option == CURLOPT_FTPPORT ||
              option == CURLOPT_COOKIE ||
              option == CURLOPT_SSLCERT ||
              option == CURLOPT_SSLCERTPASSWD ||
              option == CURLOPT_COOKIEFILE ||
              option == CURLOPT_CUSTOMREQUEST ||
              option == CURLOPT_INTERFACE ||
              option == CURLOPT_KRB4LEVEL ||
              option == CURLOPT_CAINFO ||
              option == CURLOPT_CAPATH ||
              option == CURLOPT_RANDOM_FILE ||
              option == CURLOPT_COOKIEJAR ||
              option == CURLOPT_SSL_CIPHER_LIST ||
              option == CURLOPT_EGDSOCKET ||
              option == CURLOPT_SSLCERTTYPE ||
              option == CURLOPT_SSLKEY ||
              option == CURLOPT_SSLKEYTYPE ||
              option == CURLOPT_SSLKEYPASSWD ||
              option == CURLOPT_SSLENGINE))
            {
                PyErr_SetString(PyExc_TypeError, "strings are not supported for this option");
                return NULL;
            }
        /* Free previously allocated memory to option */
        opt_masked = option % CURLOPTTYPE_OBJECTPOINT;
        if (self->options[opt_masked] != NULL) {
            free(self->options[opt_masked]);
            self->options[opt_masked] = NULL;
        }
        /* Allocate memory to hold the string */
        buf = strdup(stringdata);
        if (buf == NULL) {
            return PyErr_NoMemory();
        }
        /* Call setopt */
        res = curl_easy_setopt(self->handle, option, buf);
        /* Check for errors */
        if (res != CURLE_OK) {
            free(buf);
            CURLERROR();
        }
        self->options[opt_masked] = buf;
        Py_INCREF(Py_None);
        return Py_None;
    }

    PyErr_Clear();

    /* Handle the case of integer arguments */
    if (PyArg_ParseTuple(args, "il:setopt", &option, &longdata)) {
        /* Check that option is integer as well as the input data */
        if (option >= CURLOPTTYPE_OBJECTPOINT && option != CURLOPT_FILETIME) {
            PyErr_SetString(PyExc_TypeError, "integers are not supported for this option");
            return NULL;
        }
        res = curl_easy_setopt(self->handle, option, longdata);
        /* Check for errors */
        if (res != CURLE_OK) {
            CURLERROR();
        }
        Py_INCREF(Py_None);
        return Py_None;
    }

    PyErr_Clear();

    /* Handle the case of file objects */
    if (PyArg_ParseTuple(args, "iO!:setopt", &option, &PyFile_Type, &obj)) {
        /* Ensure the option specified a file as well as the input */
        if (!(option == CURLOPT_WRITEDATA ||
              option == CURLOPT_READDATA ||
              option == CURLOPT_WRITEHEADER ||
              option == CURLOPT_PROGRESSDATA ||
              option == CURLOPT_PASSWDDATA))
            {
                PyErr_SetString(PyExc_TypeError, "files are not supported for this option");
                return NULL;
            }
        if (option == CURLOPT_WRITEHEADER) {
            if (self->w_cb != NULL) {
                PyErr_SetString(ErrorObject, "cannot combine WRITEHEADER with WRITEFUNCTION.");
                return NULL;
            }
        }
        fp = PyFile_AsFile(obj);
        if (fp == NULL) {
            PyErr_SetString(PyExc_TypeError, "second argument must be open file");
            return NULL;
        }
        res = curl_easy_setopt(self->handle, option, fp);
        /* Check for errors */
        if (res != CURLE_OK) {
            CURLERROR();
        }
        /* Increment reference to file object and register reference in curl object */
        Py_INCREF(obj);
        if (option == CURLOPT_WRITEDATA) {
            Py_XDECREF(self->writedata);
            self->writedata = obj;
        }
        if (option == CURLOPT_READDATA) {
            Py_XDECREF(self->readdata);
            self->readdata = obj;
        }
        if (option == CURLOPT_WRITEHEADER) {
            Py_XDECREF(self->writeheader);
            self->writeheader = obj;
            self->writeheader_set = 1;
        }
        /* Return success */
        Py_INCREF(Py_None);
        return Py_None;
    }

    PyErr_Clear();

    /* Handle the case of list objects */
    if (PyArg_ParseTuple(args, "iO!:setopt", &option, &PyList_Type, &obj)) {
        struct curl_slist **slist = NULL;

        switch (option) {
        case CURLOPT_HTTPHEADER:
            slist = &self->httpheader;
            break;
        case CURLOPT_QUOTE:
            slist = &self->quote;
            break;
        case CURLOPT_POSTQUOTE:
            slist = &self->postquote;
            break;
        case CURLOPT_PREQUOTE:
            slist = &self->prequote;
            break;
        case CURLOPT_HTTPPOST:
            slist = NULL;
            break;
        default:
            /* None of the list options were recognized, throw exception */
            PyErr_SetString(PyExc_TypeError, "lists are not supported for this option");
            return NULL;
        }

        len = PyList_Size(obj);
        if (len == 0) {
            /* Empty list - do nothing */
            Py_INCREF(Py_None);
            return Py_None;
        }

        /* Handle HTTPPOST different since we construct a HttpPost form struct */
        if (option == CURLOPT_HTTPPOST) {
            struct curl_httppost *last = NULL;

            /* Free previously allocated httppost */
            curl_formfree(self->httppost);
            self->httppost = NULL;

            for (i = 0; i < len; i++) {
                PyObject *listitem = PyList_GetItem(obj, i);
                if (!PyString_Check(listitem)) {
                    curl_formfree(self->httppost);
                    self->httppost = NULL;
                    PyErr_SetString(PyExc_TypeError, "list items must be string objects");
                    return NULL;
                }
                str = PyString_AsString(listitem);
                res = curl_formparse(str, &self->httppost, &last);
                if (res != CURLE_OK) {
                    curl_formfree(self->httppost);
                    self->httppost = NULL;
                    CURLERROR();
                }
            }
            res = curl_easy_setopt(self->handle, CURLOPT_HTTPPOST, self->httppost);
            /* Check for errors */
            if (res != CURLE_OK) {
                curl_formfree(self->httppost);
                self->httppost = NULL;
                CURLERROR();
            }
            Py_INCREF(Py_None);
            return Py_None;
        }

        /* Just to be sure we do not bug off here */
        assert(slist != NULL);

        /* Free previously allocated list */
        curl_slist_free_all(*slist);
        *slist = NULL;

        /* Handle regular list operations on the other options */
        for (i = 0; i < len; i++) {
            PyObject *listitem = PyList_GetItem(obj, i);
            struct curl_slist *nlist;

            if (!PyString_Check(listitem)) {
                curl_slist_free_all(*slist);
                *slist = NULL;
                PyErr_SetString(PyExc_TypeError, "list items must be string objects");
                return NULL;
            }
            /* INFO: curl_slist_append() internally does strdup() the data */
            str = PyString_AsString(listitem);
            nlist = curl_slist_append(*slist, str);
            if (nlist == NULL || nlist->data == NULL) {
                curl_slist_free_all(*slist);
                *slist = NULL;
                return PyErr_NoMemory();
            }
            *slist = nlist;
        }
        res = curl_easy_setopt(self->handle, option, *slist);
        /* Check for errors */
        if (res != CURLE_OK) {
            curl_slist_free_all(*slist);
            *slist = NULL;
            CURLERROR();
        }
        Py_INCREF(Py_None);
        return Py_None;
    }

    PyErr_Clear();

    /* Handle the case of function objects for callbacks */
    if (PyArg_ParseTuple(args, "iO!:setopt", &option, &PyFunction_Type, &obj) ||
        PyArg_ParseTuple(args, "iO!:setopt", &option, &PyCFunction_Type, &obj) ||
        PyArg_ParseTuple(args, "iO!:setopt", &option, &PyMethod_Type, &obj))
      {
        /* We use function types here to make sure that our callback
         * definitions exactly match the <curl/curl.h> interface.
         */
        const curl_write_callback w_cb = write_callback;
        const curl_read_callback r_cb = read_callback;
        const curl_write_callback h_cb = header_callback;
        const curl_progress_callback pro_cb = progress_callback;
        const curl_passwd_callback pwd_cb = password_callback;
        const curl_debug_callback d_cb = debug_callback;

        PyErr_Clear();

        switch(option) {
        case CURLOPT_WRITEFUNCTION:
            if (self->writeheader_set == 1) {
                PyErr_SetString(ErrorObject, "cannot combine WRITEFUNCTION with WRITEHEADER option.");
                return NULL;
            }
            Py_INCREF(obj);
            Py_XDECREF(self->writedata);
            Py_XDECREF(self->w_cb);
            self->w_cb = obj;
            curl_easy_setopt(self->handle, CURLOPT_WRITEFUNCTION, w_cb);
            curl_easy_setopt(self->handle, CURLOPT_WRITEDATA, self);
            break;
        case CURLOPT_READFUNCTION:
            Py_INCREF(obj);
            Py_XDECREF(self->readdata);
            Py_XDECREF(self->r_cb);
            self->r_cb = obj;
            curl_easy_setopt(self->handle, CURLOPT_READFUNCTION, r_cb);
            curl_easy_setopt(self->handle, CURLOPT_READDATA, self);
            break;
        case CURLOPT_HEADERFUNCTION:
            Py_INCREF(obj);
            Py_XDECREF(self->h_cb);
            self->h_cb = obj;
            curl_easy_setopt(self->handle, CURLOPT_HEADERFUNCTION, h_cb);
            curl_easy_setopt(self->handle, CURLOPT_WRITEHEADER, self);
            break;
        case CURLOPT_PROGRESSFUNCTION:
            Py_INCREF(obj);
            Py_XDECREF(self->pro_cb);
            self->pro_cb = obj;
            curl_easy_setopt(self->handle, CURLOPT_PROGRESSFUNCTION, pro_cb);
            curl_easy_setopt(self->handle, CURLOPT_PROGRESSDATA, self);
            break;
        case CURLOPT_PASSWDFUNCTION:
            Py_INCREF(obj);
            Py_XDECREF(self->pwd_cb);
            self->pwd_cb = obj;
            curl_easy_setopt(self->handle, CURLOPT_PASSWDFUNCTION, pwd_cb);
            curl_easy_setopt(self->handle, CURLOPT_PASSWDDATA, self);
            break;
        case CURLOPT_DEBUGFUNCTION:
            Py_INCREF(obj);
            Py_XDECREF(self->d_cb);
            self->d_cb = obj;
            curl_easy_setopt(self->handle, CURLOPT_DEBUGFUNCTION, d_cb);
            curl_easy_setopt(self->handle, CURLOPT_DEBUGDATA, self);
            break;
        default:
            /* None of the list options were recognized, throw exception */
            PyErr_SetString(PyExc_TypeError, "functions are not supported for this option");
            return NULL;
        }
        Py_INCREF(Py_None);
        return Py_None;
    }

    PyErr_Clear();

    /* Failed to match any of the function signatures -- return error */
    PyErr_SetString(PyExc_TypeError, "invalid arguments to setopt");
    return NULL;
}


static PyObject *
do_curl_getinfo(CurlObject *self, PyObject *args)
{
    int option;
    int res;

    /* Check that we have a valid curl handle for the object */
    if (self->handle == NULL) {
        PyErr_SetString(ErrorObject, "cannot invoke getinfo, no curl handle");
        return NULL;
    }
    if (get_thread_state(self) != NULL) {
        PyErr_SetString(ErrorObject, "cannot invoke getinfo, perform() is running");
        return NULL;
    }

    /* Parse option */
    if (!PyArg_ParseTuple(args, "i:getinfo", &option)) {
        return NULL;
    }

    if (option == CURLINFO_HEADER_SIZE ||
        option == CURLINFO_REQUEST_SIZE ||
        option == CURLINFO_SSL_VERIFYRESULT ||
        option == CURLINFO_FILETIME ||
        option == CURLINFO_REDIRECT_COUNT ||
        option == CURLINFO_HTTP_CODE)
    {
        long l_res = -1;

        /* Return long as result */
        res = curl_easy_getinfo(self->handle, option, &l_res);
        /* Check for errors and return result */
        if (res != CURLE_OK) {
            CURLERROR();
        }
        return PyLong_FromLong(l_res);
    }

    if (option == CURLINFO_EFFECTIVE_URL ||
        option == CURLINFO_CONTENT_TYPE)
    {
        char *s_res = NULL;

        /* Return string as result */
        res = curl_easy_getinfo(self->handle, option, &s_res);
        /* Check for errors and return result */
        if (res != CURLE_OK) {
            CURLERROR();
        }
        /* If the resulting string is NULL, return None */
        if (s_res == NULL) {
            Py_INCREF(Py_None);
            return Py_None;
        }
        else {
            return PyString_FromString(s_res);
        }
    }

    if (option == CURLINFO_TOTAL_TIME ||
        option == CURLINFO_NAMELOOKUP_TIME ||
        option == CURLINFO_CONNECT_TIME ||
        option == CURLINFO_PRETRANSFER_TIME ||
        option == CURLINFO_STARTTRANSFER_TIME ||
        option == CURLINFO_SIZE_UPLOAD ||
        option == CURLINFO_SIZE_DOWNLOAD ||
        option == CURLINFO_SPEED_DOWNLOAD ||
        option == CURLINFO_SPEED_UPLOAD ||
        option == CURLINFO_CONTENT_LENGTH_DOWNLOAD ||
        option == CURLINFO_REDIRECT_TIME ||
        option == CURLINFO_CONTENT_LENGTH_UPLOAD)
    {
        double d_res = 0.0;

        /* Return float as result */
        res = curl_easy_getinfo(self->handle, option, &d_res);
        /* Check for errors and return result */
        if (res != CURLE_OK) {
            CURLERROR();
        }
        return PyFloat_FromDouble(d_res);
    }

    /* Got wrong option on the method call */
    PyErr_SetString(PyExc_ValueError, "invalid argument to getinfo");
    return NULL;
}


/*************************************************************************
// CurlMultiObject
**************************************************************************/

/* --------------- construct/destruct (i.e. open/close) --------------- */

/* constructor - this is a module-level function returning a new instance */
static CurlMultiObject *
do_multi_new(PyObject *dummy, PyObject *args)
{
    CurlMultiObject *self;

    UNUSED(dummy);
    if (!PyArg_ParseTuple(args, ":CurlMulti")) {
        return NULL;
    }

    /* Allocate python curl-multi object */
#if defined(USE_GC)
    self = (CurlMultiObject *) PyObject_GC_New(CurlMultiObject, &CurlMulti_Type);
    if (self) PyObject_GC_Track(self);
#elif (PY_VERSION_HEX >= 0x01060000)
    self = (CurlMultiObject *) PyObject_New(CurlMultiObject, &CurlMulti_Type);
#else
    self = (CurlMultiObject *) PyObject_NEW(CurlMultiObject, &CurlMulti_Type);
#endif
    if (self == NULL)
        return NULL;

    /* Initialize object attributes */
    self->dict = NULL;
    self->state = NULL;

    /* Allocate libcurl multi handle */
    self->multi_handle = curl_multi_init();
    if (self->multi_handle == NULL) {
        Py_DECREF(self);
        PyErr_SetString(ErrorObject, "initializing curl-multi failed");
        return NULL;
    }
    return self;
}

static void
util_multi_close(CurlMultiObject *self)
{
    assert(self != NULL);
    self->state = NULL;
    if (self->multi_handle != NULL) {
        CURLM *multi_handle = self->multi_handle;
        self->multi_handle = NULL;
        curl_multi_cleanup(multi_handle);
    }
}

static void
do_multi_dealloc(CurlMultiObject *self)
{
#if defined(USE_GC)
    PyObject_GC_UnTrack(self);
    Py_TRASHCAN_SAFE_BEGIN(self)
#endif

    Py_XDECREF(self->dict);
    self->dict = NULL;
    util_multi_close(self);

#if defined(USE_GC)
    PyObject_GC_Del(self);
#elif (PY_VERSION_HEX >= 0x01060000)
    PyObject_Del(self);
#else
    PyMem_DEL(self);
#endif

#if defined(USE_GC)
    Py_TRASHCAN_SAFE_END(self)
#endif
}

static PyObject *
do_multi_close(CurlMultiObject *self, PyObject *args)
{
    if (!PyArg_ParseTuple(args, ":close")) {
        return NULL;
    }
    if (self->state != NULL) {
        PyErr_SetString(ErrorObject, "cannot invoke close, perform() is running");
        return NULL;
    }
    util_multi_close(self);
    Py_INCREF(Py_None);
    return Py_None;
}


/* --------------- GC support --------------- */

#if defined(USE_GC)

/* Drop references that may have created reference cycles. */
static int
do_multi_clear(CurlMultiObject *self)
{
    Py_XDECREF(self->dict);
    self->dict = NULL;
    return 0;
}

static int
do_multi_traverse(CurlMultiObject *self, visitproc visit, void *arg)
{
    int err;

    if (self->dict != NULL) {
        if ((err = visit(self->dict, arg)) != 0)
            return err;
    }
    return 0;
}

#endif /* USE_GC */


/* --------------- perform --------------- */

static PyObject *
do_multi_perform(CurlMultiObject *self, PyObject *args)
{
    int res = -1;
    int running = -1;

    /* Sanity checks */
    if (!PyArg_ParseTuple(args, ":perform")) {
        return NULL;
    }

    if (self->multi_handle == NULL) {
        PyErr_SetString(ErrorObject, "cannot invoke perform, no curl-multi handle");
        return NULL;
    }
    if (self->state != NULL) {
        PyErr_SetString(ErrorObject, "cannot invoke perform - already running");
        return NULL;
    }

    /* Release global lock and start */
    self->state = PyThreadState_Get();
    assert(self->state != NULL);
    Py_BEGIN_ALLOW_THREADS
    res = curl_multi_perform(self->multi_handle, &running);
    Py_END_ALLOW_THREADS
    self->state = NULL;

    /* We assume these errors are ok, otherwise throw exception */
    if (res != CURLM_OK && res != CURLM_CALL_MULTI_PERFORM) {
        CURLERROR2("perform failed");
    }

    /* Return a tuple with the result and the number of running handles */
    return Py_BuildValue("(ii)", res, running);
}


/* --------------- add_handle/remove_handle --------------- */

/* static utility function */
static int
check_multi_handle(const CurlMultiObject *self, const CurlObject *obj)
{
    /* check CurlMultiObject status */
    assert_curl_multi_object(self);
    if (self->multi_handle == NULL) {
        PyErr_SetString(ErrorObject, "cannot add/remove handle - multi-stack is closed");
        return -1;
    }
    if (self->state != NULL) {
        PyErr_SetString(ErrorObject, "cannot add/remove handle - multi_perform() already running");
        return -1;
    }
    /* check CurlObject status */
    assert_curl_object(obj);
    if (obj->state != NULL) {
        PyErr_SetString(ErrorObject, "cannot add/remove handle - perform() of curl object already running");
        return -1;
    }
    if (obj->multi_stack != NULL && obj->multi_stack != self) {
        PyErr_SetString(ErrorObject, "cannot add/remove handle - curl object already on another multi-stack");
        return -1;
    }
    return 0;
}

static PyObject *
do_multi_add_handle(CurlMultiObject *self, PyObject *args)
{
    CurlObject *obj;
    int res;

    if (!PyArg_ParseTuple(args, "O!:add_handle", &Curl_Type, &obj)) {
        return NULL;
    }
    if (check_multi_handle(self, obj) != 0) {
        return NULL;
    }
    if (obj->handle == NULL) {
        PyErr_SetString(ErrorObject, "curl object already closed");
        return NULL;
    }
    if (obj->multi_stack == self) {
        PyErr_SetString(ErrorObject, "curl object already on this multi-stack");
        return NULL;
    }
    assert(obj->multi_stack == NULL);
    res = curl_multi_add_handle(self->multi_handle, obj->handle);
    if (res != CURLM_CALL_MULTI_PERFORM) {
        CURLERROR2("add_handle failed");
    }
    obj->multi_stack = self;
    Py_INCREF(self);
    Py_INCREF(Py_None);
    return Py_None;
}

static PyObject *
do_multi_remove_handle(CurlMultiObject *self, PyObject *args)
{
    CurlObject *obj;
    int res;

    if (!PyArg_ParseTuple(args, "O!:remove_handle", &Curl_Type, &obj)) {
        return NULL;
    }
    if (check_multi_handle(self, obj) != 0) {
        return NULL;
    }
    if (obj->handle == NULL) {
        /* handle already closed -- ignore */
        goto done;
    }
    if (obj->multi_stack != self) {
        PyErr_SetString(ErrorObject, "curl object not on this multi-stack");
        return NULL;
    }
    res = curl_multi_remove_handle(self->multi_handle, obj->handle);
    if (res != CURLM_OK) {
        CURLERROR2("remove_handle failed");
    }
    assert(obj->multi_stack == self);
    obj->multi_stack = NULL;
    Py_DECREF(self);
done:
    Py_INCREF(Py_None);
    return Py_None;
}


/* --------------- fdset ---------------------- */

static PyObject *
do_multi_fdset(CurlMultiObject *self, PyObject *args)
{
    CURLMcode res = -1;
    int max_fd, fd;
    PyObject *fdset_list = NULL, *read_list = NULL, *write_list = NULL,
             *except_list = NULL, *py_fd = NULL;

    /* Sanity checks */
    if (!PyArg_ParseTuple(args, ":fdset")) {
        return NULL;
    }

    if (self->multi_handle == NULL) {
        PyErr_SetString(ErrorObject, "cannot invoke fdset, no curl-multi handle");
        return NULL;
    }

    /* Allocate lists */
    if ((fdset_list = PyTuple_New(3)) == NULL) goto error;
    if ((read_list = PyList_New(0)) == NULL) goto error;
    if ((write_list = PyList_New(0)) == NULL) goto error;
    if ((except_list = PyList_New(0)) == NULL) goto error;

    if (PyTuple_SetItem(fdset_list, 0, read_list) == -1) goto error;
    if (PyTuple_SetItem(fdset_list, 1, write_list) == -1) goto error;
    if (PyTuple_SetItem(fdset_list, 2, except_list) == -1) goto error;

    /* Clear file descriptor sets */
    FD_ZERO(&self->read_fd_set);
    FD_ZERO(&self->write_fd_set);
    FD_ZERO(&self->exc_fd_set);

    /* Don't bother releasing the gil as this is just a data structure operation */
    res = curl_multi_fdset(self->multi_handle, &self->read_fd_set, &self->write_fd_set,
                           &self->exc_fd_set, &max_fd);

    /* We assume these errors are ok, otherwise throw exception */
    if (res != CURLM_OK) goto error;

    /* Populate lists */
    for (fd = 0; fd < max_fd + 1; fd++) {
        if (FD_ISSET(fd, &self->read_fd_set)) {
            if ((py_fd = PyInt_FromLong((long)fd)) == NULL) goto error;
            if (PyList_Append(read_list, py_fd) == -1) goto error;
        }
        if (FD_ISSET(fd, &self->write_fd_set)) {
            if ((py_fd = PyInt_FromLong((long)fd)) == NULL) goto error;
            if (PyList_Append(write_list, py_fd) == -1) goto error;
        }
        if (FD_ISSET(fd, &self->exc_fd_set)) {
            if ((py_fd = PyInt_FromLong((long)fd)) == NULL) goto error;
            if (PyList_Append(except_list, py_fd) == -1) goto error;
        }
    }
    return fdset_list;

    /* We reached this part of the code due to some error */
error:
    Py_XDECREF(py_fd);
    Py_XDECREF(read_list);
    Py_XDECREF(write_list);
    Py_XDECREF(except_list);
    Py_XDECREF(fdset_list);

    CURLERROR2("fdset failed due to internal errors");
}

/* --------------- select --------------- */

static PyObject *
do_multi_select(CurlMultiObject *self, PyObject *args)
{
    int max_fd, n;
    double timeout;
    long seconds;
    struct timeval tv, *tvp;
    PyObject *tout = Py_None;
    CURLMcode res = -1;

    /* Sanity checks */
    if (!PyArg_ParseTuple(args, "O:select", &tout)) {
        return NULL;
    }

    if (self->multi_handle == NULL) {
        PyErr_SetString(ErrorObject, "cannot invoke fdset, no curl-multi handle");
        return NULL;
    }

    if (tout == Py_None)
        tvp = (struct timeval *)0;
    else if (!PyArg_Parse(tout, "d", &timeout)) {
                PyErr_SetString(PyExc_TypeError,
                                "timeout must be a float or None");
                return NULL;
        }
        else {
                if (timeout > (double)LONG_MAX) {
                        PyErr_SetString(PyExc_OverflowError,
                                        "timeout period too long");
                        return NULL;
                }
                seconds = (long)timeout;
                timeout = timeout - (double)seconds;
                tv.tv_sec = seconds;
                tv.tv_usec = (long)(timeout*1000000.0);
                tvp = &tv;
        }

    FD_ZERO(&self->read_fd_set);
    FD_ZERO(&self->write_fd_set);
    FD_ZERO(&self->exc_fd_set);

    res = curl_multi_fdset(self->multi_handle, &self->read_fd_set,
                           &self->write_fd_set, &self->exc_fd_set, &max_fd);
    if (res != CURLM_OK) {
        CURLERROR2("multi_fdset failed");
    }

    Py_BEGIN_ALLOW_THREADS
    n = select(max_fd + 1, &self->read_fd_set, &self->write_fd_set, &self->exc_fd_set, tvp);
    Py_END_ALLOW_THREADS

    return PyInt_FromLong(n);
}


/*************************************************************************
// type definitions
**************************************************************************/

/* --------------- methods --------------- */

static char co_cleanup_doc [] = "cleanup() -> None.  Deprecated, use close().\n";
static char co_close_doc [] = "close() -> None.  Close handle and end curl session.\n";
#if 0
static char co_copy_doc [] = "copy() -> New curl object. FIXME\n";
#endif
static char co_perform_doc [] = "perform() -> None.  Perform a file transfer.  Throws pycurl.error exception upon failure.\n";
static char co_setopt_doc [] = "setopt(option, parameter) -> None.  Set curl session options.  Throws pycurl.error exception upon failure.\n";
static char co_getinfo_doc [] = "getinfo(info) -> Res.  Extract and return information from a curl session.  Throws pycurl.error exception upon failure.\n";
static char co_multi_fdset_doc [] = "fdset() -> Tuple.  Returns a tuple of three lists that can be passed to the select.select() method .\n";
static char co_multi_select_doc [] = "select(timeout) -> Int.  Returns result from doing a select() on the curl multi file descriptor with the given timeout.\n";

static PyMethodDef curlobject_methods[] = {
    {"cleanup", (PyCFunction)do_curl_close, METH_VARARGS, co_cleanup_doc},
    {"close", (PyCFunction)do_curl_close, METH_VARARGS, co_close_doc},
#if 0
    {"copy", (PyCFunction)do_curl_copy, METH_VARARGS, co_copy_doc},
#endif
    {"perform", (PyCFunction)do_curl_perform, METH_VARARGS, co_perform_doc},
    {"setopt", (PyCFunction)do_curl_setopt, METH_VARARGS, co_setopt_doc},
    {"getinfo", (PyCFunction)do_curl_getinfo, METH_VARARGS, co_getinfo_doc},
    {NULL, NULL, 0, NULL}
};

static PyMethodDef curlmultiobject_methods[] = {
    {"cleanup", (PyCFunction)do_multi_close, METH_VARARGS, NULL},
    {"close", (PyCFunction)do_multi_close, METH_VARARGS, NULL},
    {"perform", (PyCFunction)do_multi_perform, METH_VARARGS, NULL},
    {"add_handle", (PyCFunction)do_multi_add_handle, METH_VARARGS, NULL},
    {"remove_handle", (PyCFunction)do_multi_remove_handle, METH_VARARGS, NULL},
    {"fdset", (PyCFunction)do_multi_fdset, METH_VARARGS, co_multi_fdset_doc},
    {"select", (PyCFunction)do_multi_select, METH_VARARGS, co_multi_select_doc},
    {NULL, NULL, 0, NULL}
};


/* --------------- setattr/getattr --------------- */

static PyObject *curlobject_constants = NULL;
static PyObject *curlmultiobject_constants = NULL;

static int
my_setattr(PyObject **dict, char *name, PyObject *v)
{
    if (v == NULL) {
        int rv = -1;
        if (*dict != NULL)
            rv = PyDict_DelItemString(*dict, name);
        if (rv < 0)
            PyErr_SetString(PyExc_AttributeError, "delete non-existing attribute");
        return rv;
    }
    if (*dict == NULL) {
        *dict = PyDict_New();
        if (*dict == NULL)
            return -1;
    }
    return PyDict_SetItemString(*dict, name, v);
}

static PyObject *
my_getattr(PyObject *co, char *name, PyObject *dict1, PyObject *dict2, PyMethodDef *m)
{
    PyObject *v = NULL;
    if (v == NULL && dict1 != NULL)
        v = PyDict_GetItemString(dict1, name);
    if (v == NULL && dict2 != NULL)
        v = PyDict_GetItemString(dict2, name);
    if (v != NULL) {
        Py_INCREF(v);
        return v;
    }
    return Py_FindMethod(m, co, name);
}

static int
do_curl_setattr(CurlObject *co, char *name, PyObject *v)
{
    assert_curl_object(co);
    return my_setattr(&co->dict, name, v);
}

static int
do_multi_setattr(CurlMultiObject *co, char *name, PyObject *v)
{
    assert_curl_multi_object(co);
    return my_setattr(&co->dict, name, v);
}

static PyObject *
do_curl_getattr(CurlObject *co, char *name)
{
    assert_curl_object(co);
    return my_getattr((PyObject *)co, name, co->dict,
                      curlobject_constants, curlobject_methods);
}

static PyObject *
do_multi_getattr(CurlMultiObject *co, char *name)
{
    assert_curl_multi_object(co);
    return my_getattr((PyObject *)co, name, co->dict,
                      curlmultiobject_constants, curlmultiobject_methods);
}


/* --------------- actual type definitions --------------- */

statichere PyTypeObject Curl_Type = {
    PyObject_HEAD_INIT(NULL)
    0,                          /* ob_size */
    "Curl",                     /* tp_name */
    sizeof(CurlObject),         /* tp_basicsize */
    0,                          /* tp_itemsize */
    /* Methods */
    (destructor)do_curl_dealloc,   /* tp_dealloc */
    0,                          /* tp_print */
    (getattrfunc)do_curl_getattr,  /* tp_getattr */
    (setattrfunc)do_curl_setattr,  /* tp_setattr */
    0,                          /* tp_compare */
    0,                          /* tp_repr */
    0,                          /* tp_as_number */
    0,                          /* tp_as_sequence */
    0,                          /* tp_as_mapping */
    0,                          /* tp_hash */
    0,                          /* tp_call */
    0,                          /* tp_str */
    0,                          /* tp_getattro */
    0,                          /* tp_setattro */
    0,                          /* tp_as_buffer */
#if defined(USE_GC)
    Py_TPFLAGS_HAVE_GC,         /* tp_flags */
    0,                          /* tp_doc */
    (traverseproc)do_curl_traverse, /* tp_traverse */
    (inquiry)do_curl_clear      /* tp_clear */
#else
    0                           /* tp_flags */
#endif
    /* More fields follow here, depending on your Python version. You can
     * safely ignore any compiler warnings about missing initializers.
     */
};

statichere PyTypeObject CurlMulti_Type = {
    PyObject_HEAD_INIT(NULL)
    0,                          /* ob_size */
    "CurlMulti",                /* tp_name */
    sizeof(CurlMultiObject),    /* tp_basicsize */
    0,                          /* tp_itemsize */
    /* Methods */
    (destructor)do_multi_dealloc,   /* tp_dealloc */
    0,                          /* tp_print */
    (getattrfunc)do_multi_getattr,  /* tp_getattr */
    (setattrfunc)do_multi_setattr,  /* tp_setattr */
    0,                          /* tp_compare */
    0,                          /* tp_repr */
    0,                          /* tp_as_number */
    0,                          /* tp_as_sequence */
    0,                          /* tp_as_mapping */
    0,                          /* tp_hash */
    0,                          /* tp_call */
    0,                          /* tp_str */
    0,                          /* tp_getattro */
    0,                          /* tp_setattro */
    0,                          /* tp_as_buffer */
#if defined(USE_GC)
    Py_TPFLAGS_HAVE_GC,         /* tp_flags */
    0,                          /* tp_doc */
    (traverseproc)do_multi_traverse, /* tp_traverse */
    (inquiry)do_multi_clear     /* tp_clear */
#else
    0                           /* tp_flags */
#endif
    /* More fields follow here, depending on your Python version. You can
     * safely ignore any compiler warnings about missing initializers.
     */
};


/*************************************************************************
// module level
// Note that the object constructors (do_curl_new, do_multi_new)
// are module-level functions as well.
**************************************************************************/

static PyObject *
do_global_init(PyObject *dummy, PyObject *args)
{
    int res, option;

    UNUSED(dummy);
    if (!PyArg_ParseTuple(args, "i:global_init", &option)) {
        return NULL;
    }

    if (!(option == CURL_GLOBAL_SSL ||
          option == CURL_GLOBAL_WIN32 ||
          option == CURL_GLOBAL_ALL ||
          option == CURL_GLOBAL_NOTHING)) {
        PyErr_SetString(PyExc_ValueError, "invalid option to global_init");
        return NULL;
    }

    res = curl_global_init(option);
    if (res != CURLE_OK) {
        PyErr_SetString(ErrorObject, "unable to set global option");
        return NULL;
    }

    Py_INCREF(Py_None);
    return Py_None;
}


static PyObject *
do_global_cleanup(PyObject *dummy, PyObject *args)
{
    UNUSED(dummy);
    if (!PyArg_ParseTuple(args, ":global_cleanup")) {
        return NULL;
    }

    curl_global_cleanup();
    Py_INCREF(Py_None);
    return Py_None;
}



static PyObject *vi_str(const char *s)
{
    if (s == NULL)
        s = "";
    while (*s == ' ')
        s++;
    return PyString_FromString(s);
}

static PyObject *
do_version_info(PyObject *dummy, PyObject *args)
{
    const curl_version_info_data *vi;
    PyObject *t;
    PyObject *protocols;
    int i;

    UNUSED(dummy);
    if (!PyArg_ParseTuple(args, ":version_info")) {
        return NULL;
    }
    vi = curl_version_info(CURLVERSION_NOW);
    if (vi == NULL) {
        PyErr_SetString(ErrorObject, "unable to get version info");
        return NULL;
    }

    for (i = 0; vi->protocols[i] != NULL; )
        i++;
    protocols = PyTuple_New(i);
    if (protocols == NULL)
        return NULL;
    for (i = 0; vi->protocols[i] != NULL; i++) {
        PyTuple_SET_ITEM(protocols, i, vi_str(vi->protocols[i]));
    }

    t = PyTuple_New(9);
    if (t == NULL)
        return NULL;
    PyTuple_SET_ITEM(t, 0, PyInt_FromLong((long) vi->age));
    PyTuple_SET_ITEM(t, 1, vi_str(vi->version));
    PyTuple_SET_ITEM(t, 2, PyInt_FromLong((long) vi->version_num));
    PyTuple_SET_ITEM(t, 3, vi_str(vi->host));
    PyTuple_SET_ITEM(t, 4, PyInt_FromLong(vi->features));
    PyTuple_SET_ITEM(t, 5, vi_str(vi->ssl_version));
    PyTuple_SET_ITEM(t, 6, PyInt_FromLong(vi->ssl_version_num));
    PyTuple_SET_ITEM(t, 7, vi_str(vi->libz_version));
    PyTuple_SET_ITEM(t, 8, protocols);
    return t;
}


/* Per function docstrings */
static char pycurl_global_init_doc [] =
"global_init(option) -> None.  Initialize curl environment.\n";

static char pycurl_global_cleanup_doc [] =
"global_cleanup() -> None.  Cleanup curl environment.\n";

static char pycurl_version_info_doc [] =
"version_info() -> tuple.  Returns a 9-tuple with the version info.\n";

static char pycurl_curl_new_doc [] =
"Curl() -> New curl object.  Implicitly calls global_init() if not called.\n";
static char pycurl_curl_init_doc [] =
"init() -> New curl object.   Deprecated, use Curl() instead.\n";

static char pycurl_multi_new_doc [] =
"CurlMulti() -> New curl multi-object.\n";
static char pycurl_multi_init_doc [] =
"multi_init() -> New curl multi-object. Deprecated, use CurlMulti() instead.\n";


/* List of functions defined in the curl module */
static PyMethodDef curl_methods[] = {
    {"global_init", (PyCFunction)do_global_init, METH_VARARGS, pycurl_global_init_doc},
    {"global_cleanup", (PyCFunction)do_global_cleanup, METH_VARARGS, pycurl_global_cleanup_doc},
    {"version_info", (PyCFunction)do_version_info, METH_VARARGS, pycurl_version_info_doc},
    {"Curl", (PyCFunction)do_curl_new, METH_VARARGS, pycurl_curl_new_doc},
    {"init", (PyCFunction)do_curl_new, METH_VARARGS, pycurl_curl_init_doc},
    {"CurlMulti", (PyCFunction)do_multi_new, METH_VARARGS, pycurl_multi_new_doc},
    {"multi_init", (PyCFunction)do_multi_new, METH_VARARGS, pycurl_multi_init_doc},
    {NULL, NULL, 0, NULL}
};


/* Module docstring */
static char module_doc [] =
"This module implements an interface to the cURL library.\n"
"\n"
"Types:\n"
"\n"
"Curl() -> New object.  Create a new curl object.\n"
"CurlMulti() -> New object.  Create a new curl multi-object.\n"
"\n"
"Functions:\n"
"\n"
"global_init(option) -> None.  Initialize curl environment.\n"
"global_cleanup() -> None.  Cleanup curl environment.\n"
"version_info() -> tuple.  Return version information.\n"
;


/* Helper functions for inserting constants into the module namespace */

static void
insobj2(PyObject *dict1, PyObject *dict2, char *name, PyObject *value)
{
    /* Insert an object into one or two dicts. Eats a reference to value.
     * See also the implementation of PyDict_SetItemString(). */
    PyObject *key = NULL;

    if (dict1 == NULL && dict2 == NULL)
        goto error;
    if (value == NULL)
        goto error;
    key = PyString_FromString(name);
    if (key == NULL)
        goto error;
#if 0
    PyString_InternInPlace(&key);   /* XXX Should we really? */
#endif
    if (dict1 != NULL) {
        assert(PyDict_GetItem(dict1, key) == NULL);
        if (PyDict_SetItem(dict1, key, value) != 0)
            goto error;
    }
    if (dict2 != NULL && dict2 != dict1) {
        assert(PyDict_GetItem(dict2, key) == NULL);
        if (PyDict_SetItem(dict2, key, value) != 0)
            goto error;
    }
    Py_DECREF(key);
    Py_DECREF(value);
    return;
error:
    Py_FatalError("pycurl: insobj2");
    assert(0);
}

static void
insstr(PyObject *d, char *name, char *value)
{
    insobj2(d, NULL, name, PyString_FromString(value));
}

static void
insint(PyObject *d, char *name, long value)
{
    insobj2(d, NULL, name, PyInt_FromLong(value));
}

static void
insint_c(PyObject *d, char *name, long value)
{
    insobj2(d, curlobject_constants, name, PyInt_FromLong(value));
}

#if 0
static void
insint_m(PyObject *d, char *name, long value)
{
    insobj2(d, curlmultiobject_constants, name, PyInt_FromLong(value));
}
#endif


/* Initialization function for the module */
DL_EXPORT(void)
    initpycurl(void)
{
    PyObject *m, *d;

    /* Initialize the type of the new type object here; doing it here
     * is required for portability to Windows without requiring C++. */
    Curl_Type.ob_type = &PyType_Type;
    CurlMulti_Type.ob_type = &PyType_Type;

    /* Create the module and add the functions */
    m = Py_InitModule3("pycurl", curl_methods, module_doc);
    assert(m != NULL && PyModule_Check(m));

    /* Add error object to the module */
    d = PyModule_GetDict(m);
    assert(d != NULL);
    ErrorObject = PyErr_NewException("pycurl.error", NULL, NULL);
    assert(ErrorObject != NULL);
    PyDict_SetItemString(d, "error", ErrorObject);

    curlobject_constants = PyDict_New();
    assert(curlobject_constants != NULL);

    /* Add version strings to the module */
    insstr(d, "version", curl_version());
    insstr(d, "COMPILE_DATE", __DATE__ " " __TIME__);
    insint(d, "COMPILE_PY_VERSION_HEX", PY_VERSION_HEX);
    insint(d, "COMPILE_LIBCURL_VERSION_NUM", LIBCURL_VERSION_NUM);

    /**
     ** the order of these constants mostly follows <curl/curl.h>
     **/

    /* curl_infotype: the kind of data that is passed to information_callback */
    insint_c(d, "TEXT", CURLINFO_TEXT);
    insint_c(d, "HEADER_IN", CURLINFO_HEADER_IN);
    insint_c(d, "HEADER_OUT", CURLINFO_HEADER_OUT);
    insint_c(d, "DATA_IN", CURLINFO_DATA_IN);
    insint_c(d, "DATA_OUT", CURLINFO_DATA_OUT);

    /* CURLcode: error codes */
/* FIXME: lots of error codes are missing */
    insint_c(d, "E_OK", CURLE_OK);
    insint_c(d, "E_PROTOCOL", CURLE_UNSUPPORTED_PROTOCOL);

    /* curl_proxytype */
    insint_c(d, "PROXY_HTTP", CURLPROXY_HTTP);
    insint_c(d, "PROXY_SOCKS4", CURLPROXY_SOCKS4);
    insint_c(d, "PROXY_SOCKS5", CURLPROXY_SOCKS5);

    /* CURLoption: symbolic constants for setopt() */
/* FIXME: reorder these to match <curl/curl.h> */
    insint_c(d, "FILE", CURLOPT_WRITEDATA);
    insint_c(d, "INFILE", CURLOPT_READDATA);
    insint_c(d, "WRITEDATA", CURLOPT_WRITEDATA);
    insint_c(d, "WRITEFUNCTION", CURLOPT_WRITEFUNCTION);
    insint_c(d, "READDATA", CURLOPT_READDATA);
    insint_c(d, "READFUNCTION", CURLOPT_READFUNCTION);
    insint_c(d, "INFILESIZE", CURLOPT_INFILESIZE);
    insint_c(d, "URL", CURLOPT_URL);
    insint_c(d, "PROXY", CURLOPT_PROXY);
    insint_c(d, "PROXYPORT", CURLOPT_PROXYPORT);
    insint_c(d, "HTTPPROXYTUNNEL", CURLOPT_HTTPPROXYTUNNEL);
    insint_c(d, "VERBOSE", CURLOPT_VERBOSE);
    insint_c(d, "HEADER", CURLOPT_HEADER);
    insint_c(d, "NOPROGRESS", CURLOPT_NOPROGRESS);
    insint_c(d, "NOBODY", CURLOPT_NOBODY);
    insint_c(d, "FAILNOERROR", CURLOPT_FAILONERROR);
    insint_c(d, "UPLOAD", CURLOPT_UPLOAD);
    insint_c(d, "POST", CURLOPT_POST);
    insint_c(d, "FTPLISTONLY", CURLOPT_FTPLISTONLY);
    insint_c(d, "FTPAPPEND", CURLOPT_FTPAPPEND);
    insint_c(d, "NETRC", CURLOPT_NETRC);
    insint_c(d, "FOLLOWLOCATION", CURLOPT_FOLLOWLOCATION);
    insint_c(d, "TRANSFERTEXT", CURLOPT_TRANSFERTEXT);
    insint_c(d, "PUT", CURLOPT_PUT);
    insint_c(d, "USERPWD", CURLOPT_USERPWD);
    insint_c(d, "PROXYUSERPWD", CURLOPT_PROXYUSERPWD);
    insint_c(d, "RANGE", CURLOPT_RANGE);
    insint_c(d, "TIMEOUT", CURLOPT_TIMEOUT);
    insint_c(d, "POSTFIELDS", CURLOPT_POSTFIELDS);
    insint_c(d, "POSTFIELDSIZE", CURLOPT_POSTFIELDSIZE);
    insint_c(d, "REFERER", CURLOPT_REFERER);
    insint_c(d, "USERAGENT", CURLOPT_USERAGENT);
    insint_c(d, "FTPPORT", CURLOPT_FTPPORT);
    insint_c(d, "LOW_SPEED_LIMIT", CURLOPT_LOW_SPEED_LIMIT);
    insint_c(d, "LOW_SPEED_TIME", CURLOPT_LOW_SPEED_TIME);
    insint_c(d, "CURLOPT_RESUME_FROM", CURLOPT_RESUME_FROM);
    insint_c(d, "COOKIE", CURLOPT_COOKIE);
    insint_c(d, "HTTPHEADER", CURLOPT_HTTPHEADER);
    insint_c(d, "HTTPPOST", CURLOPT_HTTPPOST);
    insint_c(d, "SSLCERT", CURLOPT_SSLCERT);
    insint_c(d, "SSLCERTPASSWD", CURLOPT_SSLCERTPASSWD);
    insint_c(d, "CRLF", CURLOPT_CRLF);
    insint_c(d, "QUOTE", CURLOPT_QUOTE);
    insint_c(d, "POSTQUOTE", CURLOPT_POSTQUOTE);
    insint_c(d, "PREQUOTE", CURLOPT_PREQUOTE);
    insint_c(d, "WRITEHEADER", CURLOPT_WRITEHEADER);
    insint_c(d, "HEADERFUNCTION", CURLOPT_HEADERFUNCTION);
    insint_c(d, "COOKIEFILE", CURLOPT_COOKIEFILE);
    insint_c(d, "SSLVERSION", CURLOPT_SSLVERSION);
    insint_c(d, "TIMECONDITION", CURLOPT_TIMECONDITION);
    insint_c(d, "TIMEVALUE", CURLOPT_TIMEVALUE);
    insint_c(d, "CUSTOMREQUEST", CURLOPT_CUSTOMREQUEST);
    insint_c(d, "STDERR", CURLOPT_STDERR);
    insint_c(d, "INTERFACE", CURLOPT_INTERFACE);
    insint_c(d, "KRB4LEVEL", CURLOPT_KRB4LEVEL);
    insint_c(d, "PROGRESSFUNCTION", CURLOPT_PROGRESSFUNCTION);
    insint_c(d, "PROGRESSDATA", CURLOPT_PROGRESSDATA);
    insint_c(d, "SSL_VERIFYPEER", CURLOPT_SSL_VERIFYPEER);
    insint_c(d, "CAPATH", CURLOPT_CAINFO);
    insint_c(d, "CAINFO", CURLOPT_CAINFO);
    insint_c(d, "PASSWDFUNCTION", CURLOPT_PASSWDFUNCTION);
    insint_c(d, "PASSWDDATA", CURLOPT_PASSWDDATA);
    insint_c(d, "OPT_FILETIME", CURLOPT_FILETIME);
    insint_c(d, "MAXREDIRS", CURLOPT_MAXREDIRS);
    insint_c(d, "MAXCONNECTS", CURLOPT_MAXCONNECTS);
    insint_c(d, "CLOSEPOLICY", CURLOPT_CLOSEPOLICY);
    insint_c(d, "FRESH_CONNECT", CURLOPT_FRESH_CONNECT);
    insint_c(d, "FORBID_REUSE", CURLOPT_FORBID_REUSE);
    insint_c(d, "RANDOM_FILE", CURLOPT_RANDOM_FILE);
    insint_c(d, "EGDSOCKET", CURLOPT_EGDSOCKET);
    insint_c(d, "CONNECTTIMEOUT", CURLOPT_CONNECTTIMEOUT);
    insint_c(d, "HTTPGET", CURLOPT_HTTPGET);
    insint_c(d, "SSL_VERIFYHOST", CURLOPT_SSL_VERIFYHOST);
    insint_c(d, "COOKIEJAR", CURLOPT_COOKIEJAR);
    insint_c(d, "SSL_CIPHER_LIST", CURLOPT_SSL_CIPHER_LIST);
    insint_c(d, "HTTP_VERSION", CURLOPT_HTTP_VERSION);
    insint_c(d, "HTTP_VERSION_1_0", CURL_HTTP_VERSION_1_0);
    insint_c(d, "HTTP_VERSION_1_1", CURL_HTTP_VERSION_1_1);
    insint_c(d, "FTP_USE_EPSV", CURLOPT_FTP_USE_EPSV);
    insint_c(d, "SSLCERTTYPE", CURLOPT_SSLCERTTYPE);
    insint_c(d, "SSLKEY", CURLOPT_SSLKEY);
    insint_c(d, "SSLKEYTYPE", CURLOPT_SSLKEYTYPE);
    insint_c(d, "SSLKEYPASSWD", CURLOPT_SSLKEYPASSWD);
    insint_c(d, "SSLENGINE", CURLOPT_SSLENGINE);
    insint_c(d, "SSLENGINE_DEFAULT", CURLOPT_SSLENGINE_DEFAULT);
    insint_c(d, "DNS_CACHE_TIMEOUT", CURLOPT_DNS_CACHE_TIMEOUT);
    insint_c(d, "DNS_USE_GLOBAL_CACHE", CURLOPT_DNS_USE_GLOBAL_CACHE);
    insint_c(d, "DEBUGFUNCTION", CURLOPT_DEBUGFUNCTION);
    insint_c(d, "BUFFERSIZE", CURLOPT_BUFFERSIZE);
    insint_c(d, "NOSIGNAL", CURLOPT_NOSIGNAL);
    insint_c(d, "SHARE", CURLOPT_SHARE);
    insint_c(d, "PROXYTYPE", CURLOPT_PROXYTYPE);
    insint_c(d, "ENCODING", CURLOPT_ENCODING);

    /* CURL_NETRC_OPTION: constants for setopt(NETRC, x) */
    insint_c(d, "NETRC_OPTIONAL", CURL_NETRC_OPTIONAL);
    insint_c(d, "NETRC_IGNORED", CURL_NETRC_IGNORED);
    insint_c(d, "NETRC_REQUIRED", CURL_NETRC_REQUIRED);

    /* curl_TimeCond: constants for setopt(TIMECONDITION, x) */
    insint_c(d, "TIMECOND_IFMODSINCE", CURL_TIMECOND_IFMODSINCE);
    insint_c(d, "TIMECOND_IFUNMODSINCE", CURL_TIMECOND_IFUNMODSINCE);

    /* CURLINFO: symbolic constants for getinfo */
    insint_c(d, "EFFECTIVE_URL", CURLINFO_EFFECTIVE_URL);
    insint_c(d, "HTTP_CODE", CURLINFO_HTTP_CODE);
    insint_c(d, "TOTAL_TIME", CURLINFO_TOTAL_TIME);
    insint_c(d, "NAMELOOKUP_TIME", CURLINFO_NAMELOOKUP_TIME);
    insint_c(d, "CONNECT_TIME", CURLINFO_CONNECT_TIME);
    insint_c(d, "PRETRANSFER_TIME", CURLINFO_PRETRANSFER_TIME);
    insint_c(d, "SIZE_UPLOAD", CURLINFO_SIZE_UPLOAD);
    insint_c(d, "SIZE_DOWNLOAD", CURLINFO_SIZE_DOWNLOAD);
    insint_c(d, "SPEED_DOWNLOAD", CURLINFO_SPEED_DOWNLOAD);
    insint_c(d, "SPEED_UPLOAD", CURLINFO_SPEED_UPLOAD);
    insint_c(d, "HEADER_SIZE", CURLINFO_HEADER_SIZE);
    insint_c(d, "REQUEST_SIZE", CURLINFO_REQUEST_SIZE);
    insint_c(d, "SSL_VERIFYRESULT", CURLINFO_SSL_VERIFYRESULT);
    insint_c(d, "INFO_FILETIME", CURLINFO_FILETIME);
    insint_c(d, "CONTENT_LENGTH_DOWNLOAD", CURLINFO_CONTENT_LENGTH_DOWNLOAD);
    insint_c(d, "CONTENT_LENGTH_UPLOAD", CURLINFO_CONTENT_LENGTH_UPLOAD);
    insint_c(d, "STARTTRANSFER_TIME", CURLINFO_STARTTRANSFER_TIME);
    insint_c(d, "CONTENT_TYPE", CURLINFO_CONTENT_TYPE);
    insint_c(d, "REDIRECT_TIME", CURLINFO_REDIRECT_TIME);
    insint_c(d, "REDIRECT_COUNT", CURLINFO_REDIRECT_COUNT);

    /* curl_closepolicy: constants for setopt(CLOSEPOLICY, x) */
    insint_c(d, "CLOSEPOLICY_OLDEST", CURLCLOSEPOLICY_OLDEST);
    insint_c(d, "CLOSEPOLICY_LEAST_RECENTLY_USED", CURLCLOSEPOLICY_LEAST_RECENTLY_USED);
    insint_c(d, "CLOSEPOLICY_LEAST_TRAFFIC", CURLCLOSEPOLICY_LEAST_TRAFFIC);
    insint_c(d, "CLOSEPOLICY_SLOWEST", CURLCLOSEPOLICY_SLOWEST);
    insint_c(d, "CLOSEPOLICY_CALLBACK", CURLCLOSEPOLICY_CALLBACK);

    /* options for global_init() */
    insint(d, "GLOBAL_SSL", CURL_GLOBAL_SSL);
    insint(d, "GLOBAL_WIN32", CURL_GLOBAL_WIN32);
    insint(d, "GLOBAL_ALL", CURL_GLOBAL_ALL);
    insint(d, "GLOBAL_NOTHING", CURL_GLOBAL_NOTHING);
    insint(d, "GLOBAL_DEFAULT", CURL_GLOBAL_DEFAULT);

    /* curl_locktype: XXX do we need this in pycurl ??? */

    /**
     ** the order of these constants mostly follows <curl/multi.h>
     **/

    /* CURLMcode: multi error codes */
    insint(d, "E_CALL_MULTI_PERFORM", CURLM_CALL_MULTI_PERFORM);
    insint(d, "E_MULTI_OK", CURLM_OK);
    insint(d, "E_MULTI_BAD_HANDLE", CURLM_BAD_HANDLE);
    insint(d, "E_MULTI_BAD_EASY_HANDLE", CURLM_BAD_EASY_HANDLE);
    insint(d, "E_MULTI_OUT_OF_MEMORY", CURLM_OUT_OF_MEMORY);
    insint(d, "E_MULTI_INTERNAL_ERROR", CURLM_INTERNAL_ERROR);

    /* Initialize global interpreter lock */
    PyEval_InitThreads();
}

/* vi:ts=4:et
 */
