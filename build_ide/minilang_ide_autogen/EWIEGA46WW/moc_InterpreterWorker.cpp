/****************************************************************************
** Meta object code from reading C++ file 'InterpreterWorker.h'
**
** Created by: The Qt Meta Object Compiler version 69 (Qt 6.10.3)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include "../../../InterpreterWorker.h"
#include <QtCore/qmetatype.h>

#include <QtCore/qtmochelpers.h>

#include <memory>


#include <QtCore/qxptype_traits.h>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'InterpreterWorker.h' doesn't include <QObject>."
#elif Q_MOC_OUTPUT_REVISION != 69
#error "This file was generated using the moc from 6.10.3. It"
#error "cannot be used with the include files from this version of Qt."
#error "(The moc has changed too much.)"
#endif

#ifndef Q_CONSTINIT
#define Q_CONSTINIT
#endif

QT_WARNING_PUSH
QT_WARNING_DISABLE_DEPRECATED
QT_WARNING_DISABLE_GCC("-Wuseless-cast")
namespace {
struct qt_meta_tag_ZN17InterpreterWorkerE_t {};
} // unnamed namespace

template <> constexpr inline auto InterpreterWorker::qt_create_metaobjectdata<qt_meta_tag_ZN17InterpreterWorkerE_t>()
{
    namespace QMC = QtMocConstants;
    QtMocHelpers::StringRefStorage qt_stringData {
        "InterpreterWorker",
        "outputReady",
        "",
        "text",
        "finishedOk",
        "stoppedByUser",
        "runtimeError",
        "msg",
        "line",
        "column",
        "genericError",
        "run"
    };

    QtMocHelpers::UintData qt_methods {
        // Signal 'outputReady'
        QtMocHelpers::SignalData<void(const QString &)>(1, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::QString, 3 },
        }}),
        // Signal 'finishedOk'
        QtMocHelpers::SignalData<void()>(4, 2, QMC::AccessPublic, QMetaType::Void),
        // Signal 'stoppedByUser'
        QtMocHelpers::SignalData<void()>(5, 2, QMC::AccessPublic, QMetaType::Void),
        // Signal 'runtimeError'
        QtMocHelpers::SignalData<void(const QString &, int, int)>(6, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::QString, 7 }, { QMetaType::Int, 8 }, { QMetaType::Int, 9 },
        }}),
        // Signal 'genericError'
        QtMocHelpers::SignalData<void(const QString &)>(10, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::QString, 7 },
        }}),
        // Slot 'run'
        QtMocHelpers::SlotData<void()>(11, 2, QMC::AccessPublic, QMetaType::Void),
    };
    QtMocHelpers::UintData qt_properties {
    };
    QtMocHelpers::UintData qt_enums {
    };
    return QtMocHelpers::metaObjectData<InterpreterWorker, qt_meta_tag_ZN17InterpreterWorkerE_t>(QMC::MetaObjectFlag{}, qt_stringData,
            qt_methods, qt_properties, qt_enums);
}
Q_CONSTINIT const QMetaObject InterpreterWorker::staticMetaObject = { {
    QMetaObject::SuperData::link<QObject::staticMetaObject>(),
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN17InterpreterWorkerE_t>.stringdata,
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN17InterpreterWorkerE_t>.data,
    qt_static_metacall,
    nullptr,
    qt_staticMetaObjectRelocatingContent<qt_meta_tag_ZN17InterpreterWorkerE_t>.metaTypes,
    nullptr
} };

void InterpreterWorker::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    auto *_t = static_cast<InterpreterWorker *>(_o);
    if (_c == QMetaObject::InvokeMetaMethod) {
        switch (_id) {
        case 0: _t->outputReady((*reinterpret_cast<std::add_pointer_t<QString>>(_a[1]))); break;
        case 1: _t->finishedOk(); break;
        case 2: _t->stoppedByUser(); break;
        case 3: _t->runtimeError((*reinterpret_cast<std::add_pointer_t<QString>>(_a[1])),(*reinterpret_cast<std::add_pointer_t<int>>(_a[2])),(*reinterpret_cast<std::add_pointer_t<int>>(_a[3]))); break;
        case 4: _t->genericError((*reinterpret_cast<std::add_pointer_t<QString>>(_a[1]))); break;
        case 5: _t->run(); break;
        default: ;
        }
    }
    if (_c == QMetaObject::IndexOfMethod) {
        if (QtMocHelpers::indexOfMethod<void (InterpreterWorker::*)(const QString & )>(_a, &InterpreterWorker::outputReady, 0))
            return;
        if (QtMocHelpers::indexOfMethod<void (InterpreterWorker::*)()>(_a, &InterpreterWorker::finishedOk, 1))
            return;
        if (QtMocHelpers::indexOfMethod<void (InterpreterWorker::*)()>(_a, &InterpreterWorker::stoppedByUser, 2))
            return;
        if (QtMocHelpers::indexOfMethod<void (InterpreterWorker::*)(const QString & , int , int )>(_a, &InterpreterWorker::runtimeError, 3))
            return;
        if (QtMocHelpers::indexOfMethod<void (InterpreterWorker::*)(const QString & )>(_a, &InterpreterWorker::genericError, 4))
            return;
    }
}

const QMetaObject *InterpreterWorker::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *InterpreterWorker::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_staticMetaObjectStaticContent<qt_meta_tag_ZN17InterpreterWorkerE_t>.strings))
        return static_cast<void*>(this);
    return QObject::qt_metacast(_clname);
}

int InterpreterWorker::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QObject::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;
    if (_c == QMetaObject::InvokeMetaMethod) {
        if (_id < 6)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 6;
    }
    if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 6)
            *reinterpret_cast<QMetaType *>(_a[0]) = QMetaType();
        _id -= 6;
    }
    return _id;
}

// SIGNAL 0
void InterpreterWorker::outputReady(const QString & _t1)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 0, nullptr, _t1);
}

// SIGNAL 1
void InterpreterWorker::finishedOk()
{
    QMetaObject::activate(this, &staticMetaObject, 1, nullptr);
}

// SIGNAL 2
void InterpreterWorker::stoppedByUser()
{
    QMetaObject::activate(this, &staticMetaObject, 2, nullptr);
}

// SIGNAL 3
void InterpreterWorker::runtimeError(const QString & _t1, int _t2, int _t3)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 3, nullptr, _t1, _t2, _t3);
}

// SIGNAL 4
void InterpreterWorker::genericError(const QString & _t1)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 4, nullptr, _t1);
}
QT_WARNING_POP
