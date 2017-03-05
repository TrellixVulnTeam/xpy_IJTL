﻿#include <Python.h>
#include "xpy.h"
#include "xpyhelp.h"
#include "log.h"
#include "fmt/format.h"
#include <string>
using namespace xpy;

#define MAXRET 256

static csharp_callback sharp_cb = nullptr;

static PyObject *func_proxy = nullptr;
static PyObject *func_object = nullptr;
static PyObject *func_garbage = nullptr;

int init_csharp_python_funcs(csharp_callback cb)
{
    sharp_cb = cb;

    PyObject *pModule, *pClass;
    int ret = 1;

    pModule = PyImport_ImportModule("sharp");
    if (pModule != NULL)
    {
        pClass = PyObject_GetAttrString(pModule, "sharp");
        if (pClass)
        {
            func_proxy = PyObject_GetAttrString(pClass, "_proxy");
            if (!(func_proxy && PyCallable_Check(func_proxy)))
            {
                logger::error("Cannot find method \"{}\"", "_proxy");
                ret = -1;
            }

            func_object = PyObject_GetAttrString(pClass, "_object");
            if (!(func_object && PyCallable_Check(func_object)))
            {
                logger::error("Cannot find method \"{}\"", "_object");
                ret = -1;
            }

            func_garbage = PyObject_GetAttrString(pClass, "_garbage");
            if (!(func_garbage && PyCallable_Check(func_garbage)))
            {
                logger::error("Cannot find method \"{}\"", "_garbage");
                ret = -1;
            }

            Py_DECREF(pClass);
        }
        else
        {
            logger::error("Cannot find class \"{}\"", "sharp");
            Py_DECREF(pModule);
            return -1;
        }
        Py_DECREF(pModule);
    }
    else
    {
        logger::error("Failed to load module: \"{}\"", "sharp");
        return -1;
    }
    return ret;
}

int sharp_collect_garbage(int n, int *result)
{
    PyObject *pValue, *pNum;
    int ret = 0;

    for (int i = 0; i < n; i++)
    {
        pValue = PyObject_CallObject(func_garbage, NULL);

        if (pValue == NULL)
        {
            break; // ignore error message
        }

        if (Py_None == pValue)
        {
            Py_DECREF(pValue);
            break;
        }

        pNum = PyNumber_Long(pValue);

        int overflow;
        long id = PyLong_AsLongAndOverflow(pNum, &overflow);
        if (overflow)
        {
            // TODO:
        }
        else
        {
            result[ret] = id;
            ret++;
        }
        Py_DECREF(pValue);
        Py_DECREF(pNum);
    }
    return ret;
}

const char *get_python_function(const char *module, const char *funcname, int *id)
{
    PyObject *pModule, *pFunc, *pType, *pStr, *pArgs, *pValue;
    std::string s;

    const char *err = nullptr;

    *id = 0;

    pModule = PyImport_ImportModule(module);
    if (pModule != NULL)
    {
        pFunc = PyObject_GetAttrString(pModule, funcname);
        if (!pFunc)
        {
            s = fmt::format("Cannot find function \"{}\"", funcname);
            error_nc(err, s.c_str());
            Py_DECREF(pModule);
            return err;
        }
        if (!PyCallable_Check(pFunc))
        {
            pType = PyObject_Type(pFunc);
            pStr = PyObject_Str(pType);
            const char *type_name = PyUnicode_AsUTF8(pStr);
            s = fmt::format("Invalid type {} for [{}.{}]", type_name, module, funcname);
            error_nc(err, s.c_str());
            Py_DECREF(pStr);
            Py_DECREF(pType);
            Py_DECREF(pFunc);
            Py_DECREF(pModule);
            return err;
        }

        pArgs = PyTuple_New(1);
        PyTuple_SetItem(pArgs, 0, pFunc);  // PyTuple_SetItem "steals" a reference to pFunc.
        Py_INCREF(pFunc);
        pValue = PyObject_CallObject(func_proxy, pArgs);
        Py_DECREF(pArgs);

        if (pValue == NULL)
        {
            Py_DECREF(pFunc);
            Py_DECREF(pModule);
            error_nc(err, "call sharp._proxy failed");
            return err;
        }

        PyObject *pRet, *pRetValue;
        pRet = PyTuple_GetItem(pValue, 0);
        const char *type = PyUnicode_AsUTF8(pRet);

        if (type == NULL || type[0] != 'P')
        {
            Py_DECREF(pValue);
            Py_DECREF(pFunc);
            Py_DECREF(pModule);
            error_nc(err, "Not a python object");
            return err;
        }

        pRet = PyTuple_GetItem(pValue, 1);
        pRetValue = PyNumber_Long(pRet);
        long n = PyLong_AsLong(pRetValue);
        *id = n;

        Py_DECREF(pRetValue);
        Py_DECREF(pValue);
        Py_DECREF(pFunc);
        Py_DECREF(pModule);
    }
    else
    {
        error_nc(err, "Failed to load module: \"sharp\"");
        return err;
    }

    return NULL;
}

int call_python_function(int argc, var *argv, int strc, const char **strs, const char **err)
{
    assert(*err == 0);

    if (argc <= 0 || argv->type != var_type::PYTHONOBJ)
    {
        error_nc(*err, "Need Function");
        return -1;
    }

    PyObject *pFunc, *pArgs, *pPArgs, *pValue;

    pArgs = PyTuple_New(argc);
    for (int i = 0; i < argc; i++)
    {
        var v = argv[i];
        var_type t = v.type;
        switch (t)
        {
        case var_type::NONE:
            PyTuple_SetItem(pArgs, i, Py_None);
            break;
        case var_type::INTEGER:
            pValue = PyLong_FromLong(v.d);
            PyTuple_SetItem(pArgs, i, pValue);
            break;
        case var_type::INT64:
            pValue = PyLong_FromLongLong(v.d64);
            PyTuple_SetItem(pArgs, i, pValue);
            break;
        case var_type::REAL:
            pValue = PyFloat_FromDouble(v.f);
            PyTuple_SetItem(pArgs, i, pValue);
            break;
        case var_type::BOOLEAN:
            if (v.d)
                PyTuple_SetItem(pArgs, i, Py_True);
            else
                PyTuple_SetItem(pArgs, i, Py_False);
            break;
        case var_type::STRING:
            // todo: add short string cache
            if (strs)
            {
                if (v.d < 0 || v.d >= strc)
                {
                    error_nc(*err, "Invalid string id");
                    return -1;
                }
                pValue = PyUnicode_FromString(strs[v.d]);
            }
            else
            {
                pValue = PyUnicode_FromString((const char *)v.ptr);
            }
            PyTuple_SetItem(pArgs, i, pValue);
            break;
        case var_type::POINTER:
            pValue = PyCapsule_New(v.ptr, NULL, NULL);
            PyTuple_SetItem(pArgs, i, pValue);
            break;
        case var_type::PYTHONOBJ:
        case var_type::SHARPOBJ:
            PyObject *pObjectArgs, *pType, *pId;

            pObjectArgs = PyTuple_New(2);
            if (t == var_type::PYTHONOBJ)
                pType = PyUnicode_FromString("P");
            else
                pType = PyUnicode_FromString("S");
            PyTuple_SetItem(pObjectArgs, 0, pType);

            pId = PyLong_FromLong(v.d);
            PyTuple_SetItem(pObjectArgs, 1, pId);

            pValue = PyObject_CallObject(func_object, pObjectArgs);
            Py_DECREF(pObjectArgs);

            if (pValue == NULL)
            {
                error_nc(*err, "call sharp._object failed");
                return -1;
            }

            PyTuple_SetItem(pArgs, i, pValue);
            break;
        default:
            std::string s = fmt::format("Invalid type {}", (int)v.type);
            char *e = new char[s.length() + 1];
            strcpy(e, s.c_str());
            *err = e;
            return -1;
        }
    }

    pFunc = PyTuple_GetItem(pArgs, 0);
    if (argc == 1)
    {
        pValue = PyObject_CallObject(pFunc, NULL);
    }
    else
    {
        pPArgs = PySequence_GetSlice(pArgs, 1, PySequence_Size(pArgs));
        pValue = PyObject_CallObject(pFunc, pPArgs);
        Py_DECREF(pPArgs);
    }
    Py_DECREF(pArgs);

    if (pValue == NULL)
    {
        std::string s = fmt::format("Call python function failed.\n{}", fetch_py_exception_msg());
        error_nc(*err, s.c_str());
        return -1;
    }

    int marshal_arguments(var *v, PyObject *args);

    int result = marshal_arguments(argv, pValue);
    Py_DECREF(pValue);
    return result;
}

int marshal_var(var &v, PyObject *pItem)
{
    PyObject *pArgs, *pValue;

    if (Py_None == pItem)
    {
        v.type = var_type::NONE;
    }
    else if (PyBool_Check(pItem) == 1)
    {
        v.type = var_type::BOOLEAN;
        if (Py_True == pItem)
            v.d = 1;
        else
            v.d = 0;
    }
    else if (PyNumber_Check(pItem) == 1)
    {
        if (PyLong_Check(pItem) == 1)
        {
            v.type = var_type::INTEGER;
            pValue = PyNumber_Long(pItem);

            int overflow;
            long result = PyLong_AsLongAndOverflow(pValue, &overflow);
            if (overflow)
            {
                v.type = var_type::INT64;
                v.d64 = PyLong_AsLongLong(pValue);
            }
            else
            {
                v.type = var_type::INTEGER;
                v.d = result;
            }
            Py_DECREF(pValue);
        }
        else if (PyFloat_Check(pItem) == 1)
        {
            v.type = var_type::REAL;
            pValue = PyNumber_Float(pItem);
            v.f = PyFloat_AsDouble(pValue);
            Py_DECREF(pValue);
        }
        else
        {
            PyErr_SetString(PyExc_TypeError, "Unsupported PyNumber argument.");
            return -1;
        }
    }
    else if (PyUnicode_Check(pItem) == 1)
    {
        v.type = var_type::STRING;
        const char *s = PyUnicode_AsUTF8(pItem);
        char *new_s = new char[strlen(s) + 1];
        strcpy(new_s, s);
        v.str = (void *)new_s;  // 接收者释放
    }
    else if (PyCapsule_CheckExact(pItem) == 1)
    {
        v.type = var_type::POINTER;
        v.ptr = PyCapsule_GetPointer(pItem, NULL);
    }
    else
    {
        // func_proxy: call sharp._proxy to get proxy
        pArgs = PyTuple_New(1);
        PyTuple_SetItem(pArgs, 0, pItem);
        Py_INCREF(pItem);
        pValue = PyObject_CallObject(func_proxy, pArgs);
        Py_DECREF(pArgs);

        if (pValue == NULL)
        {
            return -1;  // call _proxy failed
        }

        PyObject *pRet, *pRetValue;
        pRet = PyTuple_GetItem(pValue, 0);
        const char *type = PyUnicode_AsUTF8(pRet);
        if (type == NULL)
        {
            PyErr_SetString(PyExc_RuntimeError, "Invalid proxy function.");
            return -1;
        }
        else
        {
            if (type[0] == 'S')
            {
                v.type = var_type::SHARPOBJ;
            }
            else // type[0] == 'P'
            {
                v.type = var_type::PYTHONOBJ;
            }

            pRet = PyTuple_GetItem(pValue, 1);
            pRetValue = PyNumber_Long(pRet);
            long n = PyLong_AsLong(pRetValue);
            v.d = n;
            Py_DECREF(pRetValue);
        }
        Py_DECREF(pValue);
    }
    return 1;
}

int marshal_arguments(var *v, PyObject *args)
{
    if (!PyTuple_Check(args))
    {
        if (marshal_var(v[0], args) != 1)
        {
            return -1;
        }
        return 1;
    }
    else
    {
        Py_ssize_t size = PyTuple_Size(args);

        if (size <= 0)
        {
            if (!PyErr_Occurred())
                PyErr_SetString(PyExc_TypeError, "You must supply at least one argument.");
            return -1;
        }

        PyObject *pItem;

        for (Py_ssize_t i = 0; i < size; i++)
        {
            pItem = PyTuple_GetItem(args, i); // dosen't increase refcount
            if (pItem == NULL)
            {
                return -1;
            }

            if (marshal_var(v[i], pItem) != 1)
            {
                return -1;
            }

            if (PyErr_Occurred())
            {
                return -1;
            }
        }

        return (int)size;
    }
}

static PyObject *xpy_csharpcall(PyObject *self, PyObject *args)
{
    var arg[MAXRET];
    int argsnum = marshal_arguments(arg, args);

    logger::info("xpy_csharpcall argc: {}", argsnum);
    if (argsnum == -1)
    {
        return NULL; // when use PyErr_SetString in a c extension function, you must return NULL.
    }

    string_pusher sp = {0};
    if (sharp_cb)
    {
        int rt = sharp_cb(argsnum, arg, &sp);
        // TODO: 根据具体需求完善
    }

    return Py_None;
}

static PyObject * xpy_writelog(PyObject *self, PyObject *args)
{
    int level;
    const char *msg;

    if (!PyArg_ParseTuple(args, "is", &level, &msg))
        return NULL;
    xlog(level, msg, false);
    return Py_None;
}

static PyMethodDef EmbMethods[] = {
    {"csharpcall",  xpy_csharpcall, METH_VARARGS,
     "call csharp."},
    {"writelog",  xpy_writelog, METH_VARARGS,
     "output log."},
    {NULL, NULL, 0, NULL}
};

static PyModuleDef EmbModule = {
    PyModuleDef_HEAD_INIT, "xpy", NULL, -1, EmbMethods,
    NULL, NULL, NULL, NULL
};

static PyObject *PyInit_emb(void)
{
    return PyModule_Create(&EmbModule);
}

int register_xpy_functions()
{
    PyImport_AppendInittab("xpy", &PyInit_emb);
    return 0;
}
