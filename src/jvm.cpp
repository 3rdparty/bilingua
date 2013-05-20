#include <jni.h>
#include <stdarg.h>
#include <stdlib.h> // For atexit.

#include <glog/logging.h>

#include <map>
#include <memory>
#include <sstream>
#include <vector>

#include <stout/error.hpp>
#include <stout/exit.hpp>
#include <stout/foreach.hpp>

#include "jvm.hpp"

#include "java/lang.hpp" // For java::lang::Throwable.


// Some compilers give us warnings about 'dereferencing type-punned
// pointer will break strict-aliasing rules' when we cast our JNIEnv**
// to void**. We use this function to do the magic for us.
static void** JNIENV_CAST(JNIEnv** env)
{
  return reinterpret_cast<void**>(env);
}


JNI::Env::Env(bool daemon)
  : env(NULL), detach(false)
{
  JavaVM* jvm = Jvm::get()->jvm;

  // First check if we are already attached.
  int result = jvm->GetEnv(JNIENV_CAST(&env), Jvm::get()->version);

  // If we're not attached, attach now.
  if (result == JNI_EDETACHED) {
    if (daemon) {
      jvm->AttachCurrentThreadAsDaemon(JNIENV_CAST(&env), NULL);
    } else {
      jvm->AttachCurrentThread(JNIENV_CAST(&env), NULL);
    }
    detach = true;
  }
}


JNI::Env::~Env()
{
  if (detach) {
    Jvm::get()->jvm->DetachCurrentThread();
  }
}


// Static storage and initialization.
Jvm* Jvm::instance = NULL;


void deleter()
{
  delete Jvm::instance;
}


Try<Jvm*> Jvm::create(
    const std::vector<std::string>& options,
    JNI::Version version,
    bool exceptions)
{
  // TODO(benh): Make this thread-safe.
  if (instance != NULL) {
    return Error("Java Virtual Machine already created");
  }

  JavaVMInitArgs vmArgs;
  vmArgs.version = version;
  vmArgs.ignoreUnrecognized = false;

  JavaVMOption* opts = new JavaVMOption[options.size()];
  for (size_t i = 0; i < options.size(); i++) {
    opts[i].optionString = const_cast<char*>(options[i].c_str());
  }
  vmArgs.nOptions = options.size();
  vmArgs.options = opts;

  JavaVM* jvm = NULL;
  JNIEnv* env = NULL;

  int result = JNI_CreateJavaVM(&jvm, JNIENV_CAST(&env), &vmArgs);

  if (result == JNI_ERR) {
    return Error("Failed to create JVM!");
  }

  delete[] opts;

  instance = new Jvm(jvm, version, exceptions);

  atexit(&deleter);

  return instance;
}


bool Jvm::created()
{
  return instance != NULL;
}


Jvm* Jvm::get()
{
  if (instance == NULL) {
    create();
  }
  return CHECK_NOTNULL(instance);
}


Jvm::ConstructorFinder::ConstructorFinder(const Jvm::Class& _clazz)
  : clazz(_clazz), parameters() {}


Jvm::ConstructorFinder& Jvm::ConstructorFinder::parameter(
    const Jvm::Class& clazz)
{
  parameters.push_back(clazz);
  return *this;
}


Jvm::Constructor::Constructor(const Constructor& that)
  : clazz(that.clazz), id(that.id) {}


Jvm::Constructor::Constructor(const Class& _clazz, const jmethodID _id)
  : clazz(_clazz), id(_id) {}


Jvm::MethodFinder::MethodFinder(
    const Jvm::Class& _clazz,
    const std::string& _name)
  : clazz(_clazz),
    name(_name),
    parameters() {}


Jvm::MethodFinder& Jvm::MethodFinder::parameter(const Class& type)
{
  parameters.push_back(type);
  return *this;
}


Jvm::MethodSignature Jvm::MethodFinder::returns(const Class& returnType) const
{
  return Jvm::MethodSignature(clazz, name, returnType, parameters);
}


Jvm::MethodSignature::MethodSignature(const MethodSignature& that)
  : clazz(that.clazz),
    name(that.name),
    returnType(that.returnType),
    parameters(that.parameters) {}


Jvm::MethodSignature::MethodSignature(
    const Class& _clazz,
    const std::string& _name,
    const Class& _returnType,
    const std::vector<Class>& _parameters)
  : clazz(_clazz),
    name(_name),
    returnType(_returnType),
    parameters(_parameters) {}


Jvm::Method::Method(const Method& that)
    : clazz(that.clazz), id(that.id) {}


Jvm::Method::Method(const Class& _clazz, const jmethodID _id)
    : clazz(_clazz), id(_id) {}


const Jvm::Class Jvm::Class::VOID = Jvm::Class("V");
const Jvm::Class Jvm::Class::BOOLEAN = Jvm::Class("Z");
const Jvm::Class Jvm::Class::BYTE = Jvm::Class("B");
const Jvm::Class Jvm::Class::CHAR = Jvm::Class("C");
const Jvm::Class Jvm::Class::SHORT = Jvm::Class("S");
const Jvm::Class Jvm::Class::INT = Jvm::Class("I");
const Jvm::Class Jvm::Class::LONG = Jvm::Class("J");
const Jvm::Class Jvm::Class::FLAOT = Jvm::Class("F");
const Jvm::Class Jvm::Class::DOUBLE = Jvm::Class("D");
const Jvm::Class Jvm::Class::STRING = Class::named("java/lang/String");


const Jvm::Class Jvm::Class::named(const std::string& name)
{
  return Jvm::Class(name, false /* NOT a native type. */);
}


Jvm::Class::Class(const Class& that)
  : name(that.name), native(that.native) {}


Jvm::Class::Class(const std::string& _name, bool _native)
  : name(_name), native(_native) {}


const Jvm::Class Jvm::Class::arrayOf() const
{
  return Jvm::Class("[" + name, native);
}


Jvm::ConstructorFinder Jvm::Class::constructor() const
{
  return Jvm::ConstructorFinder(*this);
}


Jvm::MethodFinder Jvm::Class::method(const std::string& name) const
{
  return Jvm::MethodFinder(*this, name);
}


std::string Jvm::Class::signature() const
{
  return native ? name : "L" + name + ";";
}


Jvm::Field::Field(const Field& that)
  : clazz(that.clazz), id(that.id) {}


Jvm::Field::Field(const Class& _clazz, const jfieldID _id)
    : clazz(_clazz), id(_id) {}


jstring Jvm::string(const std::string& s)
{
  JNI::Env env;
  return env->NewStringUTF(s.c_str());
}


Jvm::Constructor Jvm::findConstructor(const ConstructorFinder& finder)
{
  jmethodID id = findMethod(
      finder.clazz,
      "<init>",
      Jvm::Class::VOID,
      finder.parameters,
      false);

  return Jvm::Constructor(finder.clazz, id);
}


Jvm::Method Jvm::findMethod(const MethodSignature& signature)
{
  jmethodID id = findMethod(
      signature.clazz,
      signature.name,
      signature.returnType,
      signature.parameters,
      false);

  return Jvm::Method(signature.clazz, id);
}


Jvm::Method Jvm::findStaticMethod(const MethodSignature& signature)
{
  jmethodID id = findMethod(
      signature.clazz,
      signature.name,
      signature.returnType,
      signature.parameters,
      true);

  return Jvm::Method(signature.clazz, id);
}


Jvm::Field Jvm::findStaticField(const Class& clazz, const std::string& name)
{
  JNI::Env env;

  jfieldID id = env->GetStaticFieldID(
      findClass(clazz),
      name.c_str(),
      clazz.signature().c_str());

  check(env);

  return Jvm::Field(clazz, id);
}


jobject Jvm::invoke(const Constructor& ctor, ...)
{
  JNI::Env env;
  va_list args;
  va_start(args, ctor);
  jobject o = env->NewObjectV(findClass(ctor.clazz), ctor.id, args);
  va_end(args);
  check(env);
  return o;
}


template <>
jobject Jvm::getStaticField<jobject>(const Field& field)
{
  JNI::Env env;
  jobject o = env->GetStaticObjectField(findClass(field.clazz), field.id);
  check(env);
  return o;
}


template <>
bool Jvm::getStaticField<bool>(const Field& field)
{
  JNI::Env env;
  bool b = env->GetStaticBooleanField(findClass(field.clazz), field.id);
  check(env);
  return b;
}


template <>
char Jvm::getStaticField<char>(const Field& field)
{
  JNI::Env env;
  char c = env->GetStaticCharField(findClass(field.clazz), field.id);
  check(env);
  return c;
}


template <>
short Jvm::getStaticField<short>(const Field& field)
{
  JNI::Env env;
  short s = env->GetStaticShortField(findClass(field.clazz), field.id);
  check(env);
  return s;
}


template <>
int Jvm::getStaticField<int>(const Field& field)
{
  JNI::Env env;
  int i = env->GetStaticIntField(findClass(field.clazz), field.id);
  check(env);
  return i;
}


template <>
long Jvm::getStaticField<long>(const Field& field)
{
  JNI::Env env;
  long l = env->GetStaticLongField(findClass(field.clazz), field.id);
  check(env);
  return l;
}


template <>
float Jvm::getStaticField<float>(const Field& field)
{
  JNI::Env env;
  float f = env->GetStaticFloatField(findClass(field.clazz), field.id);
  check(env);
  return f;
}


template <>
double Jvm::getStaticField<double>(const Field& field)
{
  JNI::Env env;
  double d = env->GetStaticDoubleField(findClass(field.clazz), field.id);
  check(env);
  return d;
}


Jvm::Jvm(JavaVM* _jvm, JNI::Version _version, bool _exceptions)
  : jvm(_jvm), version(_version), exceptions(_exceptions) {}


Jvm::~Jvm()
{
  if (jvm->DestroyJavaVM() != 0) {
    LOG(FATAL) << "Destroying the JVM is not supported";
  }
}


jobject Jvm::newGlobalRef(const jobject object)
{
  JNI::Env env;
  return env->NewGlobalRef(object);
}


void Jvm::deleteGlobalRef(const jobject object)
{
  JNI::Env env;
  if (object != NULL) {
    env->DeleteGlobalRef(object);
  }
}


jclass Jvm::findClass(const Class& clazz)
{
  JNI::Env env;

  // TODO(John Sirois): Consider CHECK_NOTNULL -> return Option if
  // re-purposing this code outside of tests.
  return CHECK_NOTNULL(env->FindClass(clazz.name.c_str()));
}


jmethodID Jvm::findMethod(
    const Jvm::Class& clazz,
    const std::string& name,
    const Jvm::Class& returnType,
    const std::vector<Jvm::Class>& argTypes,
    bool isStatic)
{
  JNI::Env env;

  std::ostringstream signature;
  signature << "(";
  std::vector<Jvm::Class>::iterator args;
  foreach (const Jvm::Class& type, argTypes) {
    signature << type.signature();
  }
  signature << ")" << returnType.signature();

  VLOG(1) << "Looking up" << (isStatic ? " static " : " ")
          << "method " << name << signature.str();

  jmethodID id = isStatic
    ? env->GetStaticMethodID(
        findClass(clazz),
        name.c_str(),
        signature.str().c_str())
    : env->GetMethodID(
        findClass(clazz),
        name.c_str(),
        signature.str().c_str());

  // TODO(John Sirois): Consider CHECK_NOTNULL -> return Option if
  // re-purposing this code outside of tests.
  return CHECK_NOTNULL(id);
}


template<>
void Jvm::invokeV<void>(
    const jobject receiver,
    const jmethodID id,
    va_list args)
{
  JNI::Env env;
  env->CallVoidMethodV(receiver, id, args);
  check(env);
}


template <>
jobject Jvm::invokeV<jobject>(
    const jobject receiver,
    const jmethodID id,
    va_list args)
{
  JNI::Env env;
  jobject o = env->CallObjectMethodV(receiver, id, args);
  check(env);
  return o;
}


template <>
bool Jvm::invokeV<bool>(
    const jobject receiver,
    const jmethodID id,
    va_list args)
{
  JNI::Env env;
  bool b = env->CallBooleanMethodV(receiver, id, args);
  check(env);
  return b;
}


template <>
char Jvm::invokeV<char>(
    const jobject receiver,
    const jmethodID id,
    va_list args)
{
  JNI::Env env;
  char c = env->CallCharMethodV(receiver, id, args);
  check(env);
  return c;
}


template <>
short Jvm::invokeV<short>(
    const jobject receiver,
    const jmethodID id,
    va_list args)
{
  JNI::Env env;
  short s = env->CallShortMethodV(receiver, id, args);
  check(env);
  return s;
}


template <>
int Jvm::invokeV<int>(
    const jobject receiver,
    const jmethodID id,
    va_list args)
{
  JNI::Env env;
  int i = env->CallIntMethodV(receiver, id, args);
  check(env);
  return i;
}


template <>
long Jvm::invokeV<long>(
    const jobject receiver,
    const jmethodID id,
    va_list args)
{
  JNI::Env env;
  long l = env->CallLongMethodV(receiver, id, args);
  check(env);
  return l;
}


template <>
float Jvm::invokeV<float>(
    const jobject receiver,
    const jmethodID id,
    va_list args)
{
  JNI::Env env;
  float f = env->CallFloatMethodV(receiver, id, args);
  check(env);
  return f;
}


template <>
double Jvm::invokeV<double>(
    const jobject receiver,
    const jmethodID id,
    va_list args)
{
  JNI::Env env;
  double d = env->CallDoubleMethodV(receiver, id, args);
  check(env);
  return d;
}


template<>
void Jvm::invokeStaticV<void>(
    const Class& receiver,
    const jmethodID id,
    va_list args)
{
  JNI::Env env;
  env->CallStaticVoidMethodV(findClass(receiver), id, args);
  check(env);
}


template <>
jobject Jvm::invokeStaticV<jobject>(
    const Class& receiver,
    const jmethodID id,
    va_list args)
{
  JNI::Env env;
  jobject o = env->CallStaticObjectMethodV(findClass(receiver), id, args);
  check(env);
  return o;
}


template <>
bool Jvm::invokeStaticV<bool>(
    const Class& receiver,
    const jmethodID id,
    va_list args)
{
  JNI::Env env;
  bool b = env->CallStaticBooleanMethodV(findClass(receiver), id, args);
  check(env);
  return b;
}


template <>
char Jvm::invokeStaticV<char>(
    const Class& receiver,
    const jmethodID id,
    va_list args)
{
  JNI::Env env;
  char c = env->CallStaticCharMethodV(findClass(receiver), id, args);
  check(env);
  return c;
}


template <>
short Jvm::invokeStaticV<short>(
    const Class& receiver,
    const jmethodID id,
    va_list args)
{
  JNI::Env env;
  short s = env->CallStaticShortMethodV(findClass(receiver), id, args);
  check(env);
  return s;
}


template <>
int Jvm::invokeStaticV<int>(
    const Class& receiver,
    const jmethodID id,
    va_list args)
{
  JNI::Env env;
  int i = env->CallStaticIntMethodV(findClass(receiver), id, args);
  check(env);
  return i;
}


template <>
long Jvm::invokeStaticV<long>(
    const Class& receiver,
    const jmethodID id,
    va_list args)
{
  JNI::Env env;
  long l = env->CallStaticLongMethodV(findClass(receiver), id, args);
  check(env);
  return l;
}


template <>
float Jvm::invokeStaticV<float>(
    const Class& receiver,
    const jmethodID id,
    va_list args)
{
  JNI::Env env;
  float f = env->CallStaticFloatMethodV(findClass(receiver), id, args);
  check(env);
  return f;
}


template <>
double Jvm::invokeStaticV<double>(
    const Class& receiver,
    const jmethodID id,
    va_list args)
{
  JNI::Env env;
  double d = env->CallStaticDoubleMethodV(findClass(receiver), id, args);
  check(env);
  return d;
}


void Jvm::check(JNIEnv* env)
{
  if (env->ExceptionCheck() == JNI_TRUE) {
    if (!exceptions) {
      env->ExceptionDescribe();
      LOG(FATAL) << "Caught a JVM exception, not propagating";
    } else {
      java::lang::Throwable throwable;
      java::lang::Object* object = &throwable;
      object->object = env->ExceptionOccurred();
      env->ExceptionClear();
      throw throwable;
    }
  }
}


// N.B. Both Jvm::invoke<void> and Jvm::invokeStatic<void> template
// instantiations need to be defined AFTER template instantions that
// they use (i.e., Jvm::invokeV<void>, Jvm::invokeStaticV<void>).

template <>
void Jvm::invoke<void>(const jobject receiver, const Method& method, ...)
{
  va_list args;
  va_start(args, method);
  invokeV<void>(receiver, method.id, args);
  va_end(args);
}


template <>
void Jvm::invokeStatic<void>(const Method& method, ...)
{
  va_list args;
  va_start(args, method);
  invokeStaticV<void>(method.clazz, method.id, args);
  va_end(args);
}
