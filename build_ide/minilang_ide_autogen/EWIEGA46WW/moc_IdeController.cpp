/****************************************************************************
** Meta object code from reading C++ file 'IdeController.h'
**
** Created by: The Qt Meta Object Compiler version 69 (Qt 6.10.3)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include "../../../IdeController.h"
#include <QtCore/qmetatype.h>

#include <QtCore/qtmochelpers.h>

#include <memory>


#include <QtCore/qxptype_traits.h>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'IdeController.h' doesn't include <QObject>."
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
struct qt_meta_tag_ZN13IdeControllerE_t {};
} // unnamed namespace

template <> constexpr inline auto IdeController::qt_create_metaobjectdata<qt_meta_tag_ZN13IdeControllerE_t>()
{
    namespace QMC = QtMocConstants;
    QtMocHelpers::StringRefStorage qt_stringData {
        "IdeController",
        "outputReady",
        "",
        "text",
        "runOk",
        "stoppedByUser",
        "runtimeError",
        "msg",
        "line",
        "column",
        "genericError",
        "pausedAt",
        "workerFinished",
        "wasDebug",
        "vmStepInfo",
        "VMStepInfo",
        "info",
        "diagnosticsReady",
        "DiagnosticBag",
        "bag"
    };

    QtMocHelpers::UintData qt_methods {
        // Signal 'outputReady'
        QtMocHelpers::SignalData<void(const QString &)>(1, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::QString, 3 },
        }}),
        // Signal 'runOk'
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
        // Signal 'pausedAt'
        QtMocHelpers::SignalData<void(int)>(11, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::Int, 8 },
        }}),
        // Signal 'workerFinished'
        QtMocHelpers::SignalData<void(bool)>(12, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::Bool, 13 },
        }}),
        // Signal 'vmStepInfo'
        QtMocHelpers::SignalData<void(const VMStepInfo &)>(14, 2, QMC::AccessPublic, QMetaType::Void, {{
            { 0x80000000 | 15, 16 },
        }}),
        // Signal 'diagnosticsReady'
        QtMocHelpers::SignalData<void(const DiagnosticBag &)>(17, 2, QMC::AccessPublic, QMetaType::Void, {{
            { 0x80000000 | 18, 19 },
        }}),
    };
    QtMocHelpers::UintData qt_properties {
    };
    QtMocHelpers::UintData qt_enums {
    };
    return QtMocHelpers::metaObjectData<IdeController, qt_meta_tag_ZN13IdeControllerE_t>(QMC::MetaObjectFlag{}, qt_stringData,
            qt_methods, qt_properties, qt_enums);
}
Q_CONSTINIT const QMetaObject IdeController::staticMetaObject = { {
    QMetaObject::SuperData::link<QObject::staticMetaObject>(),
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN13IdeControllerE_t>.stringdata,
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN13IdeControllerE_t>.data,
    qt_static_metacall,
    nullptr,
    qt_staticMetaObjectRelocatingContent<qt_meta_tag_ZN13IdeControllerE_t>.metaTypes,
    nullptr
} };

void IdeController::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    auto *_t = static_cast<IdeController *>(_o);
    if (_c == QMetaObject::InvokeMetaMethod) {
        switch (_id) {
        case 0: _t->outputReady((*reinterpret_cast<std::add_pointer_t<QString>>(_a[1]))); break;
        case 1: _t->runOk(); break;
        case 2: _t->stoppedByUser(); break;
        case 3: _t->runtimeError((*reinterpret_cast<std::add_pointer_t<QString>>(_a[1])),(*reinterpret_cast<std::add_pointer_t<int>>(_a[2])),(*reinterpret_cast<std::add_pointer_t<int>>(_a[3]))); break;
        case 4: _t->genericError((*reinterpret_cast<std::add_pointer_t<QString>>(_a[1]))); break;
        case 5: _t->pausedAt((*reinterpret_cast<std::add_pointer_t<int>>(_a[1]))); break;
        case 6: _t->workerFinished((*reinterpret_cast<std::add_pointer_t<bool>>(_a[1]))); break;
        case 7: _t->vmStepInfo((*reinterpret_cast<std::add_pointer_t<VMStepInfo>>(_a[1]))); break;
        case 8: _t->diagnosticsReady((*reinterpret_cast<std::add_pointer_t<DiagnosticBag>>(_a[1]))); break;
        default: ;
        }
    }
    if (_c == QMetaObject::IndexOfMethod) {
        if (QtMocHelpers::indexOfMethod<void (IdeController::*)(const QString & )>(_a, &IdeController::outputReady, 0))
            return;
        if (QtMocHelpers::indexOfMethod<void (IdeController::*)()>(_a, &IdeController::runOk, 1))
            return;
        if (QtMocHelpers::indexOfMethod<void (IdeController::*)()>(_a, &IdeController::stoppedByUser, 2))
            return;
        if (QtMocHelpers::indexOfMethod<void (IdeController::*)(const QString & , int , int )>(_a, &IdeController::runtimeError, 3))
            return;
        if (QtMocHelpers::indexOfMethod<void (IdeController::*)(const QString & )>(_a, &IdeController::genericError, 4))
            return;
        if (QtMocHelpers::indexOfMethod<void (IdeController::*)(int )>(_a, &IdeController::pausedAt, 5))
            return;
        if (QtMocHelpers::indexOfMethod<void (IdeController::*)(bool )>(_a, &IdeController::workerFinished, 6))
            return;
        if (QtMocHelpers::indexOfMethod<void (IdeController::*)(const VMStepInfo & )>(_a, &IdeController::vmStepInfo, 7))
            return;
        if (QtMocHelpers::indexOfMethod<void (IdeController::*)(const DiagnosticBag & )>(_a, &IdeController::diagnosticsReady, 8))
            return;
    }
}

const QMetaObject *IdeController::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *IdeController::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_staticMetaObjectStaticContent<qt_meta_tag_ZN13IdeControllerE_t>.strings))
        return static_cast<void*>(this);
    return QObject::qt_metacast(_clname);
}

int IdeController::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QObject::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;
    if (_c == QMetaObject::InvokeMetaMethod) {
        if (_id < 9)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 9;
    }
    if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 9)
            *reinterpret_cast<QMetaType *>(_a[0]) = QMetaType();
        _id -= 9;
    }
    return _id;
}

// SIGNAL 0
void IdeController::outputReady(const QString & _t1)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 0, nullptr, _t1);
}

// SIGNAL 1
void IdeController::runOk()
{
    QMetaObject::activate(this, &staticMetaObject, 1, nullptr);
}

// SIGNAL 2
void IdeController::stoppedByUser()
{
    QMetaObject::activate(this, &staticMetaObject, 2, nullptr);
}

// SIGNAL 3
void IdeController::runtimeError(const QString & _t1, int _t2, int _t3)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 3, nullptr, _t1, _t2, _t3);
}

// SIGNAL 4
void IdeController::genericError(const QString & _t1)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 4, nullptr, _t1);
}

// SIGNAL 5
void IdeController::pausedAt(int _t1)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 5, nullptr, _t1);
}

// SIGNAL 6
void IdeController::workerFinished(bool _t1)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 6, nullptr, _t1);
}

// SIGNAL 7
void IdeController::vmStepInfo(const VMStepInfo & _t1)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 7, nullptr, _t1);
}

// SIGNAL 8
void IdeController::diagnosticsReady(const DiagnosticBag & _t1)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 8, nullptr, _t1);
}
QT_WARNING_POP
