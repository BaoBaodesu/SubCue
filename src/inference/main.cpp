// 嵌入的 Python 仅提供发行版 DLL；避免 Python.h 在 MSVC Debug 下自动链接调试版 Python。
#ifdef _DEBUG
#undef _DEBUG
#include <Python.h>
#define _DEBUG 1
#else
#include <Python.h>
#endif

#include <filesystem>
#include <string>
#include <vector>

#ifndef SUBCUE_INFERENCE_PYTHON_HOME
#error SUBCUE_INFERENCE_PYTHON_HOME is required
#endif
#ifndef SUBCUE_INFERENCE_SITE_PACKAGES
#error SUBCUE_INFERENCE_SITE_PACKAGES is required
#endif
#ifndef SUBCUE_INFERENCE_WORKER_PATH
#error SUBCUE_INFERENCE_WORKER_PATH is required
#endif

int wmain(int argc, wchar_t *argv[])
{
    const std::filesystem::path executableDirectory = std::filesystem::absolute(argv[0]).parent_path();
    const std::filesystem::path bundledRuntime = executableDirectory / L"runtime";
    const std::filesystem::path bundledWorker = executableDirectory / L"asr_python_worker.py";
    const std::filesystem::path pythonHomePath = std::filesystem::exists(bundledRuntime / L"python312.dll")
        ? bundledRuntime : std::filesystem::path(SUBCUE_INFERENCE_PYTHON_HOME);
    const std::filesystem::path sitePackagesPath = std::filesystem::exists(
        bundledRuntime / L"Lib" / L"site-packages")
        ? bundledRuntime / L"Lib" / L"site-packages"
        : std::filesystem::path(SUBCUE_INFERENCE_SITE_PACKAGES);
    const std::filesystem::path workerFile = std::filesystem::exists(bundledWorker)
        ? bundledWorker : std::filesystem::path(SUBCUE_INFERENCE_WORKER_PATH);
    PyConfig config;
    PyConfig_InitPythonConfig(&config);
    config.parse_argv = 0;
    config.install_signal_handlers = 0;
    config.write_bytecode = 0;
    const std::wstring pythonHomeValue = pythonHomePath.wstring();
    const PyStatus homeStatus = PyConfig_SetString(&config, &config.home, pythonHomeValue.c_str());
    if (PyStatus_Exception(homeStatus)) {
        PyConfig_Clear(&config);
        return 2;
    }
    const PyStatus status = Py_InitializeFromConfig(&config);
    PyConfig_Clear(&config);
    if (PyStatus_Exception(status)) return 2;

    PyObject *path = PySys_GetObject("path");
    const std::string sitePackagesValue = sitePackagesPath.string();
    PyObject *sitePackages = PyUnicode_DecodeFSDefault(sitePackagesValue.c_str());
    if (!path || !sitePackages || PyList_Insert(path, 0, sitePackages) != 0) {
        Py_XDECREF(sitePackages);
        Py_FinalizeEx();
        return 2;
    }
    Py_DECREF(sitePackages);

    std::vector<std::wstring> arguments;
    const std::string workerValue = workerFile.string();
    wchar_t *workerPath = Py_DecodeLocale(workerValue.c_str(), nullptr);
    if (!workerPath) {
        Py_FinalizeEx();
        return 2;
    }
    arguments.emplace_back(workerPath);
    PyMem_RawFree(workerPath);
    for (int index = 1; index < argc; ++index) arguments.emplace_back(argv[index]);
    std::vector<wchar_t *> pythonArgv;
    pythonArgv.reserve(arguments.size());
    for (std::wstring &argument : arguments) pythonArgv.push_back(argument.data());
#pragma warning(push)
#pragma warning(disable: 4996)
    PySys_SetArgvEx(static_cast<int>(pythonArgv.size()), pythonArgv.data(), 0);
#pragma warning(pop)

    const std::string command = "import runpy; runpy.run_path(r'" +
        workerValue + "', run_name='__main__')";
    const int result = PyRun_SimpleString(command.c_str());
    // Torch 的 Windows CUDA 运行时在 Py_FinalizeEx 阶段可能触发静态析构崩溃；
    // Worker 是短生命周期独立进程，刷新输出后由操作系统统一回收更可靠。
    PyRun_SimpleString("import sys; sys.stdout.flush(); sys.stderr.flush()");
    return result != 0 ? 1 : 0;
}
