/*
 *      Copyright (C) 2013 Team XBMC
 *      http://kodi.tv
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with XBMC; see the file COPYING.  If not, see
 *  <http://www.gnu.org/licenses/>.
 *
 */

// python.h should always be included first before any other includes
#include <Python.h>
#include <iterator>
#include <osdefs.h>

#include "PythonInvoker.h"
#include "Application.h"
#include "ServiceBroker.h"
#include "messaging/ApplicationMessenger.h"
#include "addons/AddonManager.h"
#include "dialogs/GUIDialogKaiToast.h"
#include "filesystem/File.h"
#include "filesystem/SpecialProtocol.h"
#include "guilib/GUIComponent.h"
#include "guilib/GraphicContext.h"
#include "guilib/GUIWindowManager.h"
#include "guilib/LocalizeStrings.h"
#include "interfaces/legacy/Addon.h"
#include "interfaces/python/LanguageHook.h"
#include "interfaces/python/PyContext.h"
#include "interfaces/python/pythreadstate.h"
#include "interfaces/python/swig.h"
#include "interfaces/python/XBPython.h"
#include "threads/SingleLock.h"
#include "utils/CharsetConverter.h"
#include "utils/log.h"
#include "utils/StringUtils.h"
#include "utils/URIUtils.h"
#ifdef TARGET_POSIX
#include "platform/linux/XTimeUtils.h"
#endif

#ifdef TARGET_WINDOWS
extern "C" FILE *fopen_utf8(const char *_Filename, const char *_Mode);
#else
#define fopen_utf8 fopen
#endif

#define GC_SCRIPT \
  "import gc\n" \
  "gc.collect(2)\n"

#define PY_PATH_SEP DELIM

// Time before ill-behaved scripts are terminated
#define PYTHON_SCRIPT_TIMEOUT 5000 // ms

using namespace XFILE;
using namespace KODI::MESSAGING;

extern "C"
{
  int xbp_chdir(const char *dirname);
  char* dll_getenv(const char* szKey);
}

#define PythonModulesSize sizeof(PythonModules) / sizeof(PythonModule)

CCriticalSection CPythonInvoker::s_critical;

static const std::string getListOfAddonClassesAsString(XBMCAddon::AddonClass::Ref<XBMCAddon::Python::PythonLanguageHook>& languageHook)
{
  std::string message;
  CSingleLock l(*(languageHook.get()));
  std::set<XBMCAddon::AddonClass*>& acs = languageHook->GetRegisteredAddonClasses();
  bool firstTime = true;
  for (std::set<XBMCAddon::AddonClass*>::iterator iter = acs.begin(); iter != acs.end(); ++iter)
  {
    if (!firstTime)
      message += ",";
    else
      firstTime = false;
    message += (*iter)->GetClassname();
  }

  return message;
}

static std::vector<std::vector<wchar_t>> storeArgumentsCCompatible(std::vector<std::wstring> const & input)
{
  std::vector<std::vector<wchar_t>> output;
  std::transform(input.begin(), input.end(), std::back_inserter(output),
                [](std::wstring const & i) { return std::vector<wchar_t>(i.c_str(), i.c_str() + i.length() + 1); });

  if (output.empty())
    output.push_back(std::vector<wchar_t>(1u, '\0'));

  return output;
}

static std::vector<wchar_t *> getCPointersToArguments(std::vector<std::vector<wchar_t>> & input)
{
  std::vector<wchar_t *> output;
  std::transform(input.begin(), input.end(), std::back_inserter(output),
                [](std::vector<wchar_t> & i) { return &i[0]; });
  return output;
}

CPythonInvoker::CPythonInvoker(ILanguageInvocationHandler *invocationHandler)
  : ILanguageInvoker(invocationHandler),
    m_threadState(NULL), m_stop(false)
{ }

CPythonInvoker::~CPythonInvoker()
{
  // nothing to do for the default invoker used for registration with the
  // CScriptInvocationManager
  if (GetId() < 0)
    return;

  if (GetState() < InvokerStateDone)
    CLog::Log(LOGDEBUG, "CPythonInvoker(%d): waiting for python thread \"%s\" to stop",
      GetId(), (!m_sourceFile.empty() ? m_sourceFile.c_str() : "unknown script"));
  Stop(true);
  pulseGlobalEvent();

  onExecutionFinalized();
}

bool CPythonInvoker::Execute(const std::string &script, const std::vector<std::string> &arguments /* = std::vector<std::string>() */)
{
  if (script.empty())
    return false;

  if (!CFile::Exists(script))
  {
    CLog::Log(LOGERROR, "CPythonInvoker(%d): python script \"%s\" does not exist", GetId(), CSpecialProtocol::TranslatePath(script).c_str());
    return false;
  }

  if (!onExecutionInitialized())
    return false;

  return ILanguageInvoker::Execute(script, arguments);
}

bool CPythonInvoker::execute(const std::string &script, const std::vector<std::string> &arguments)
{
  std::vector<std::wstring> w_arguments;
  for (auto argument : arguments)
  {
    std::wstring w_argument;
    g_charsetConverter.utf8ToW(argument, w_argument);
    w_arguments.push_back(w_argument);
  }
  return execute(script, w_arguments);
}

bool CPythonInvoker::execute(const std::string &script, const std::vector<std::wstring> &arguments)
{
  // copy the code/script into a local string buffer
  m_sourceFile = script;

  // copy the arguments into a local buffer
  unsigned int argc = arguments.size();
  std::vector<std::vector<wchar_t>> argvStorage = storeArgumentsCCompatible(arguments);
  std::vector<wchar_t *> argv = getCPointersToArguments(argvStorage);

  CLog::Log(LOGDEBUG, "CPythonInvoker(%d, %s): start processing", GetId(), m_sourceFile.c_str());

  // get the global lock
  extern PyThreadState* savestate;
  PyEval_RestoreThread(savestate);
  PyThreadState* state = Py_NewInterpreter();
  if (state == NULL)
  {
    PyEval_ReleaseLock();
    CLog::Log(LOGERROR, "CPythonInvoker(%d, %s): FAILED to get thread state!", GetId(), m_sourceFile.c_str());
    return false;
  }
  // swap in my thread state
  PyThreadState_Swap(state);

  XBMCAddon::AddonClass::Ref<XBMCAddon::Python::PythonLanguageHook> languageHook(new XBMCAddon::Python::PythonLanguageHook(state->interp));
  languageHook->RegisterMe();

  onInitialization();
  setState(InvokerStateInitialized);

  std::string realFilename(CSpecialProtocol::TranslatePath(m_sourceFile));
  if (realFilename == m_sourceFile)
    CLog::Log(LOGDEBUG, "CPythonInvoker(%d, %s): the source file to load is \"%s\"", GetId(), m_sourceFile.c_str(), m_sourceFile.c_str());
  else
    CLog::Log(LOGDEBUG, "CPythonInvoker(%d, %s): the source file to load is \"%s\" (\"%s\")", GetId(), m_sourceFile.c_str(), m_sourceFile.c_str(), realFilename.c_str());

  // get path from script file name and add python path's
  // this is used for python so it will search modules from script path first
  std::string scriptDir = URIUtils::GetDirectory(realFilename);
  URIUtils::RemoveSlashAtEnd(scriptDir);
  addPath(scriptDir);

  // add all addon module dependencies to path
  if (m_addon)
  {
    std::set<std::string> paths;
    getAddonModuleDeps(m_addon, paths);
    for (std::set<std::string>::const_iterator it = paths.begin(); it != paths.end(); ++it)
      addPath(*it);
  }
  else
  { // for backwards compatibility.
    // we don't have any addon so just add all addon modules installed
    CLog::Log(LOGWARNING, "CPythonInvoker(%d): Script invoked without an addon. Adding all addon "
        "modules installed to python path as fallback. This behaviour will be removed in future "
        "version.", GetId());
    ADDON::VECADDONS addons;
    CServiceBroker::GetAddonMgr().GetAddons(addons, ADDON::ADDON_SCRIPT_MODULE);
    for (unsigned int i = 0; i < addons.size(); ++i)
      addPath(CSpecialProtocol::TranslatePath(addons[i]->LibPath()));
  }

  // we want to use sys.path so it includes site-packages
  // if this fails, default to using Py_GetPath
  PyObject *sysMod(PyImport_ImportModule((char*)"sys")); // must call Py_DECREF when finished
  PyObject *sysModDict(PyModule_GetDict(sysMod)); // borrowed ref, no need to delete
  PyObject *pathObj(PyDict_GetItemString(sysModDict, "path")); // borrowed ref, no need to delete

  if (pathObj != NULL && PyList_Check(pathObj))
  {
    for (int i = 0; i < PyList_Size(pathObj); i++)
    {
      PyObject *e = PyList_GetItem(pathObj, i); // borrowed ref, no need to delete
      if (e != NULL && PyUnicode_Check(e))
        addNativePath(PyUnicode_AsUTF8(e)); // returns internal data, don't delete or modify
    }
  }
  else
  {
      std::string GetPath;
      g_charsetConverter.wToUTF8(Py_GetPath(), GetPath);
      addNativePath(GetPath);
  }

  Py_DECREF(sysMod); // release ref to sysMod

  // set current directory and python's path.
  PySys_SetArgv(argc, &argv[0]);

  #ifdef TARGET_WINDOWS
  std::string pyPathUtf8;
  g_charsetConverter.systemToUtf8(m_pythonPath, pyPathUtf8, false);
  CLog::Log(LOGDEBUG, "CPythonInvoker(%d, %s): setting the Python path to %s", GetId(), m_sourceFile.c_str(), pyPathUtf8.c_str());
  #else // ! TARGET_WINDOWS
  CLog::Log(LOGDEBUG, "CPythonInvoker(%d, %s): setting the Python path to %s", GetId(), m_sourceFile.c_str(), m_pythonPath.c_str());
  #endif // ! TARGET_WINDOWS

  std::wstring pypath;
  g_charsetConverter.utf8ToW(m_pythonPath, pypath);
  PySys_SetPath(pypath.c_str());

  CLog::Log(LOGDEBUG, "CPythonInvoker(%d, %s): entering source directory %s", GetId(), m_sourceFile.c_str(), scriptDir.c_str());
  PyObject* module = PyImport_AddModule((char*)"__main__");
  PyObject* moduleDict = PyModule_GetDict(module);

  // when we are done initing we store thread state so we can be aborted
  PyThreadState_Swap(NULL);
  PyEval_ReleaseLock();

  // we need to check if we was asked to abort before we had inited
  bool stopping = false;
  { CSingleLock lock(m_critical);
    m_threadState = state;
    stopping = m_stop;
  }

  PyEval_AcquireThread(state);

  bool failed = false;
  std::string exceptionType, exceptionValue, exceptionTraceback;
  if (!stopping)
  {
    try
    {
      // run script from file
      // We need to have python open the file because on Windows the DLL that python
      //  is linked against may not be the DLL that xbmc is linked against so
      //  passing a FILE* to python from an fopen has the potential to crash.
      std::string nativeFilename(realFilename); // filename in system encoding
      #ifdef TARGET_WINDOWS
        if (!g_charsetConverter.utf8ToSystem(nativeFilename, true))
        {
          CLog::Log(LOGERROR, "CPythonInvoker(%d, %s): can't convert filename \"%s\" to system encoding", GetId(), m_sourceFile.c_str(), realFilename.c_str());
          return false;
        }
      #endif
    PyObject *ioMod, *openedFile;
    ioMod = PyImport_ImportModule("io");
    openedFile = PyObject_CallMethod(ioMod, "open", "ss", (char *)nativeFilename.c_str(), "r");
    Py_DECREF(ioMod);
    FILE *fp = PyFile_AsFileWithMode(openedFile, (char *)"r");

      if (fp != NULL)
      {
        PyObject *f = PyUnicode_FromString(nativeFilename.c_str());
        PyDict_SetItemString(moduleDict, "__file__", f);

        onPythonModuleInitialization(moduleDict);

        Py_DECREF(f);
        setState(InvokerStateRunning);
        XBMCAddon::Python::PyContext pycontext; // this is a guard class that marks this callstack as being in a python context
        executeScript(fp, nativeFilename, module, moduleDict);
      }
      else
        CLog::Log(LOGERROR, "CPythonInvoker(%d, %s): %s not found!", GetId(), m_sourceFile.c_str(), m_sourceFile.c_str());
    }
    catch (const XbmcCommons::Exception& e)
    {
      setState(InvokerStateFailed);
      e.LogThrowMessage();
      failed = true;
    }
    catch (...)
    {
      setState(InvokerStateFailed);
      CLog::Log(LOGERROR, "CPythonInvoker(%d, %s): failure in script", GetId(), m_sourceFile.c_str());
      failed = true;
    }
  }

  bool systemExitThrown = false;
  InvokerState stateToSet;
  if (!failed && !PyErr_Occurred())
  {
    CLog::Log(LOGINFO, "CPythonInvoker(%d, %s): script successfully run", GetId(), m_sourceFile.c_str());
    stateToSet = InvokerStateDone;
    onSuccess();
  }
  else if (PyErr_ExceptionMatches(PyExc_SystemExit))
  {
    systemExitThrown = true;
    CLog::Log(LOGINFO, "CPythonInvoker(%d, %s): script aborted", GetId(), m_sourceFile.c_str());
    stateToSet = InvokerStateFailed;
    onAbort();
  }
  else
  {
    stateToSet = InvokerStateFailed;

    // if it failed with an exception we already logged the details
    if (!failed)
    {
      PythonBindings::PythonToCppException *e = NULL;
      if (PythonBindings::PythonToCppException::ParsePythonException(exceptionType, exceptionValue, exceptionTraceback))
        e = new PythonBindings::PythonToCppException(exceptionType, exceptionValue, exceptionTraceback);
      else
        e = new PythonBindings::PythonToCppException();

      e->LogThrowMessage();
      delete e;
    }

    onError(exceptionType, exceptionValue, exceptionTraceback);
  }

  // no need to do anything else because the script has already stopped
  if (failed)
  {
    setState(stateToSet);
    return true;
  }

  PyObject *m = PyImport_AddModule((char*)"xbmc");
  if (m == NULL || PyObject_SetAttrString(m, (char*)"abortRequested", PyBool_FromLong(1)))
    CLog::Log(LOGERROR, "CPythonInvoker(%d, %s): failed to set abortRequested", GetId(), m_sourceFile.c_str());

  // make sure all sub threads have finished
  for (PyThreadState* s = state->interp->tstate_head, *old = NULL; s;)
  {
    if (s == state)
    {
      s = s->next;
      continue;
    }
    if (old != s)
    {
      CLog::Log(LOGINFO, "CPythonInvoker(%d, %s): waiting on thread %" PRIu64, GetId(), m_sourceFile.c_str(), (uint64_t)s->thread_id);
      old = s;
    }

    CPyThreadState pyState;
    Sleep(100);
    pyState.Restore();

    s = state->interp->tstate_head;
  }

  // pending calls must be cleared out
  XBMCAddon::RetardedAsyncCallbackHandler::clearPendingCalls(state);

  PyThreadState_Swap(NULL);
  PyEval_ReleaseLock();

  // set stopped event - this allows ::stop to run and kill remaining threads
  // this event has to be fired without holding m_critical
  // also the GIL (PyEval_AcquireLock) must not be held
  // if not obeyed there is still no deadlock because ::stop waits with timeout (smart one!)
  m_stoppedEvent.Set();

  { CSingleLock lock(m_critical);
    m_threadState = NULL;
  }

  PyEval_AcquireThread(state);

  onDeinitialization();

  // run the gc before finishing
  //
  // if the script exited by throwing a SystemExit exception then going back
  // into the interpreter causes this python bug to get hit:
  //    http://bugs.python.org/issue10582
  // and that causes major failures. So we are not going to go back in
  // to run the GC if that's the case.
  if (!m_stop && languageHook->HasRegisteredAddonClasses() && !systemExitThrown &&
      PyRun_SimpleString(GC_SCRIPT) == -1)
  CLog::Log(LOGERROR, "CPythonInvoker(%d, %s): failed to run the gc to clean up after running prior to shutting down the Interpreter", GetId(), m_sourceFile.c_str());

  Py_EndInterpreter(state);

  // If we still have objects left around, produce an error message detailing what's been left behind
  if (languageHook->HasRegisteredAddonClasses())
    CLog::Log(LOGWARNING, "CPythonInvoker(%d, %s): the python script \"%s\" has left several "
      "classes in memory that we couldn't clean up. The classes include: %s",
    GetId(), m_sourceFile.c_str(), m_sourceFile.c_str(), getListOfAddonClassesAsString(languageHook).c_str());

  // unregister the language hook
  languageHook->UnregisterMe();

  PyEval_ReleaseLock();

  setState(stateToSet);

  return true;
}

void CPythonInvoker::executeScript(void *fp, const std::string &script, void *module, void *moduleDict)
{
  if (fp == NULL || script.empty() || module == NULL || moduleDict == NULL)
    return;

  int m_Py_file_input = Py_file_input;
  PyRun_FileExFlags(static_cast<FILE*>(fp), script.c_str(), m_Py_file_input, static_cast<PyObject*>(moduleDict), static_cast<PyObject*>(moduleDict), 1, NULL);
}

FILE* CPythonInvoker::PyFile_AsFileWithMode(PyObject *py_file, const char *mode)
{
    FILE *f;
    PyObject *ret;
    int fd;

    ret = PyObject_CallMethod(py_file, "flush", "");
    if (ret == NULL)
        return NULL;
    Py_DECREF(ret);

    fd = PyObject_AsFileDescriptor(py_file);
    if (fd == -1)
        return NULL;

    f = fdopen(fd, mode);
    if (f == NULL) {
        PyErr_SetFromErrno(PyExc_OSError);
        return NULL;
    }

    return f;
}

bool CPythonInvoker::stop(bool abort)
{
  CSingleLock lock(m_critical);
  m_stop = true;

  if (!IsRunning())
    return false;

  setState(InvokerStateStopping);

  if (m_threadState != NULL)
  {
    PyEval_AcquireLock();
    PyThreadState* old = PyThreadState_Swap((PyThreadState*)m_threadState);

    //tell xbmc.Monitor to call onAbortRequested()
    if (m_addon != NULL)
      onAbortRequested();

    PyObject *m;
    m = PyImport_AddModule((char*)"xbmc");
    if (m == NULL || PyObject_SetAttrString(m, (char*)"abortRequested", PyBool_FromLong(1)))
      CLog::Log(LOGERROR, "CPythonInvoker(%d, %s): failed to set abortRequested", GetId(), m_sourceFile.c_str());

    PyThreadState_Swap(old);
    old = NULL;
    PyEval_ReleaseLock();

    XbmcThreads::EndTime timeout(PYTHON_SCRIPT_TIMEOUT);
    while (!m_stoppedEvent.WaitMSec(15))
    {
      if (timeout.IsTimePast())
      {
        CLog::Log(LOGERROR, "CPythonInvoker(%d, %s): script didn't stop in %d seconds - let's kill it", GetId(), m_sourceFile.c_str(), PYTHON_SCRIPT_TIMEOUT / 1000);
        break;
      }

      // We can't empty-spin in the main thread and expect scripts to be able to
      // dismantle themselves. Python dialogs aren't normal XBMC dialogs, they rely
      // on TMSG_GUI_PYTHON_DIALOG messages, so pump the message loop.
      if (g_application.IsCurrentThread())
      {
        CApplicationMessenger::GetInstance().ProcessMessages();
      }
    }

    // Useful for add-on performance metrics
    if (!timeout.IsTimePast())
      CLog::Log(LOGDEBUG, "CPythonInvoker(%d, %s): script termination took %dms", GetId(), m_sourceFile.c_str(), PYTHON_SCRIPT_TIMEOUT - timeout.MillisLeft());

    // everything which didn't exit by now gets killed
    {
      // grabbing the PyLock while holding the m_critical is asking for a deadlock
      CSingleExit ex2(m_critical);
      PyEval_AcquireLock();
    }

    // Since we released the m_critical it's possible that the state is cleaned up
    // so we need to recheck for m_threadState == NULL
    if (m_threadState != NULL)
    {
      old = PyThreadState_Swap((PyThreadState*)m_threadState);
      for (PyThreadState* state = ((PyThreadState*)m_threadState)->interp->tstate_head; state; state = state->next)
      {
        // Raise a SystemExit exception in python threads
        Py_XDECREF(state->async_exc);
        state->async_exc = PyExc_SystemExit;
        Py_XINCREF(state->async_exc);
      }

      // If a dialog entered its doModal(), we need to wake it to see the exception
      pulseGlobalEvent();
    }

    if (old != NULL)
      PyThreadState_Swap(old);

    lock.Leave();
    PyEval_ReleaseLock();
  }

  return true;
}

void CPythonInvoker::onExecutionFailed()
{
  PyThreadState_Swap(NULL);
  PyEval_ReleaseLock();

  setState(InvokerStateFailed);
  CLog::Log(LOGERROR, "CPythonInvoker(%d, %s): abnormally terminating python thread", GetId(), m_sourceFile.c_str());

  CSingleLock lock(m_critical);
  m_threadState = NULL;

  ILanguageInvoker::onExecutionFailed();
}

void CPythonInvoker::onInitialization()
{
  XBMC_TRACE;
  {
    GilSafeSingleLock lock(s_critical);
    initializeModules(getModules());
  }

  // get a possible initialization script
  const char* runscript = getInitializationScript();
  if (runscript!= NULL && strlen(runscript) > 0)
  {
    // redirecting default output to debug console
    if (PyRun_SimpleString(runscript) == -1)
      CLog::Log(LOGFATAL, "CPythonInvoker(%d, %s): initialize error", GetId(), m_sourceFile.c_str());
  }
}

void CPythonInvoker::onPythonModuleInitialization(void* moduleDict)
{
  if (m_addon.get() == NULL || moduleDict == NULL)
    return;

  PyObject *moduleDictionary = (PyObject *)moduleDict;

  PyObject *pyaddonid = PyUnicode_FromString(m_addon->ID().c_str());
  PyDict_SetItemString(moduleDictionary, "__xbmcaddonid__", pyaddonid);

  ADDON::AddonVersion version = m_addon->GetDependencyVersion("xbmc.python");
  PyObject *pyxbmcapiversion = PyUnicode_FromString(version.asString().c_str());
  PyDict_SetItemString(moduleDictionary, "__xbmcapiversion__", pyxbmcapiversion);

  PyObject *pyinvokerid = PyLong_FromLong(GetId());
  PyDict_SetItemString(moduleDictionary, "__xbmcinvokerid__", pyinvokerid);

  CLog::Log(LOGDEBUG, "CPythonInvoker(%d, %s): instantiating addon using automatically obtained id of \"%s\" dependent on version %s of the xbmc.python api",
            GetId(), m_sourceFile.c_str(), m_addon->ID().c_str(), version.asString().c_str());
}

void CPythonInvoker::onDeinitialization()
{
  XBMC_TRACE;
}

void CPythonInvoker::onError(const std::string &exceptionType /* = "" */, const std::string &exceptionValue /* = "" */, const std::string &exceptionTraceback /* = "" */)
{
  CPyThreadState releaseGil;
  CSingleLock gc(g_graphicsContext);

  CGUIDialogKaiToast *pDlgToast = CServiceBroker::GetGUI()->GetWindowManager().GetWindow<CGUIDialogKaiToast>(WINDOW_DIALOG_KAI_TOAST);
  if (pDlgToast != NULL)
  {
    std::string message;
    if (m_addon && !m_addon->Name().empty())
      message = StringUtils::Format(g_localizeStrings.Get(2102).c_str(), m_addon->Name().c_str());
    else if (m_sourceFile == CSpecialProtocol::TranslatePath("special://profile/autoexec.py"))
      message = StringUtils::Format(g_localizeStrings.Get(2102).c_str(), "autoexec.py");
    else
       message = g_localizeStrings.Get(2103);
    pDlgToast->QueueNotification(CGUIDialogKaiToast::Error, message, g_localizeStrings.Get(2104));
  }
}

void CPythonInvoker::initializeModules(const std::map<std::string, PythonModuleInitialization> &modules)
{
  for (std::map<std::string, PythonModuleInitialization>::const_iterator module = modules.begin(); module != modules.end(); ++module)
  {
    if (!initializeModule(module->second))
      CLog::Log(LOGWARNING, "CPythonInvoker(%d, %s): unable to initialize python module \"%s\"", GetId(), m_sourceFile.c_str(), module->first.c_str());
  }
}

bool CPythonInvoker::initializeModule(PythonModuleInitialization module)
{
  if (module == NULL)
    return false;

  return module() != nullptr;

}

void CPythonInvoker::getAddonModuleDeps(const ADDON::AddonPtr& addon, std::set<std::string>& paths)
{
  for (const auto& it : addon->GetDependencies())
  {
    //Check if dependency is a module addon
    ADDON::AddonPtr dependency;
    if (CServiceBroker::GetAddonMgr().GetAddon(it.id, dependency, ADDON::ADDON_SCRIPT_MODULE))
    {
      std::string path = CSpecialProtocol::TranslatePath(dependency->LibPath());
      if (paths.find(path) == paths.end())
      {
        // add it and its dependencies
        paths.insert(path);
        getAddonModuleDeps(dependency, paths);
      }
    }
  }
}

void CPythonInvoker::addPath(const std::string& path)
{
#if defined(TARGET_WINDOWS)
  if (path.empty())
    return;

  std::string nativePath(path);
  if (!g_charsetConverter.utf8ToSystem(nativePath, true))
  {
    CLog::Log(LOGERROR, "%s: can't convert UTF-8 path \"%s\" to system encoding", __FUNCTION__, path.c_str());
    return;
  }
  addNativePath(nativePath);
#else
  addNativePath(path);
#endif // defined(TARGET_WINDOWS)
}

void CPythonInvoker::addNativePath(const std::string& path)
{
  if (path.empty())
    return;

  if (!m_pythonPath.empty())
    m_pythonPath += PY_PATH_SEP;

  m_pythonPath += path;
}
