cmake_minimum_required(VERSION 3.20)
project(Xmex LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Linux 上 ld 默认 --as-needed，会把 .o 之前出现的 .so 跳过，
# 导致 xmexc 产出的 .o 找不到运行时符号。强制 --no-as-needed。
if(UNIX AND NOT APPLE)
  add_link_options(-Wl,--no-as-needed)
endif()

# Linux 适配：可执行文件和 .so 都放到同一个 bin/ 下
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# ---------------------------------------------------------------------------
# LLVM
# ---------------------------------------------------------------------------
# Linux 上 LLVM 22 也是 shared 模式，LLVMConfig.cmake 导出 `LLVM` target
# 指向 libLLVM-22.so。链接它就够了。
find_package(LLVM REQUIRED CONFIG HINTS /usr/lib/llvm-20/lib/cmake/llvm)
message(STATUS "找到 LLVM ${LLVM_PACKAGE_VERSION} (${LLVM_DIR})")

# LLVM_DEFINITIONS 是带空格的字符串,含 -D 前缀,要用 add_compile_options
# 而不是 target_compile_definitions(后者不接受 -D 前缀)。
separate_arguments(LLVM_DEFINITIONS_LIST NATIVE_COMMAND "${LLVM_DEFINITIONS}")
add_compile_options(${LLVM_DEFINITIONS_LIST})

# ---------------------------------------------------------------------------
# 运行时库
# ---------------------------------------------------------------------------
# Linux 上使用 SHARED(.so)，JIT 生成的代码通过 dlsym 解析运行时符号。
# GCC/Clang 对 -shared 默认导出全部符号，无需额外标注。
# ---------------------------------------------------------------------------
# 内置分布式项目 adviskv —— 已于 2026-10 移除
# ---------------------------------------------------------------------------
# 移除前它是 Linux-only 的子项目(xmex_runtime 直接链接它的四个静态库),
# 但核查发现 **Xmex 源码一处都没有 include 它的头** —— 那四个链接目标在
# 链接期存在,代码期没有实际使用。所以移除它不会让任何功能退化成空壳。
#
# 代价与恢复方式见 docs/raft-removal.md:adviskv 里有真 Raft(带 PreVote /
# AppendEntries / WAL / Snapshot),移除后本项目**不再具备共识能力**。
# 需要分布式存储时,把它作为外部依赖接回来(不必做子模块,普通
# find_package + 链接也可以)。

# 核心运行时:跨平台,两个平台都编。
set(XMEX_RUNTIME_SOURCES
  runtime/error.cpp
  runtime/gc.cpp
  runtime/arith.cpp
  runtime/string.cpp
  runtime/tuple.cpp
  runtime/list.cpp
  runtime/dict.cpp
  runtime/index.cpp
  runtime/method.cpp
  runtime/io.cpp
  runtime/docker.cpp
  runtime/llm.cpp
  runtime/fusion.cpp
  runtime/inference.cpp
  runtime/tokenizer.cpp
  runtime/quantize.cpp
  runtime/concurrency.cpp
  runtime/object.cpp
  runtime/trace.cpp
  # 动态库 FFI:加载本机 .dll/.so 并按描述符调用其函数。
  # **刻意跨平台** —— 加载 Windows DLL 正是它的主要用途,放进
  # if(NOT WIN32) 就等于把这个功能在 Windows 上阉掉。
  # ffi.cpp 只依赖 platform.h 里的 XMEX_DLOPEN/DLSYM 抽象(Windows 是
  # LoadLibraryA/GetProcAddress,Linux 是 dlopen/dlsym),没有 POSIX 专有依赖。
  runtime/ffi.cpp
  # ffi_api.cpp 提供 kv.ffi()/kv.ffi_list() 与实例方法注册。
  # 同样跨平台:它只依赖运行时自身的 Value/Dict/List API。
  runtime/ffi_api.cpp
  # 通用 HTTP 客户端:能打任意 URL,接口测试用。http.cpp 只依赖
  # platform.h 的 socket 抽象,所以跨平台。TLS 只在 Linux 侧可用
  # (OpenSSL 只在那个平台链),https 在 Windows 上明确报不支持。
  runtime/http.cpp
  runtime/http_api.cpp
  # 平台抽象:管道 + 子进程 spawn(Windows CreateProcess / POSIX fork+exec)。
  # FFI 隔离模式要用它起 agent 子进程。
  runtime/platform_proc.cpp
  # distributed_adapter.cpp 是纯适配层：cluster/kv 段带 _WIN32 守卫（依赖只在
  # Linux 构建的 cluster_api / kv_admin_api.cpp）。自研 raft 已于 2026-10 移除，
  # 理由与后续方案见 docs/raft-removal.md。
  runtime/distributed_adapter.cpp
)

# 仅 Linux:依赖 POSIX/seccomp/adviskv。
#
# 分两族:一族直接用了 Linux 专有头(seccomp.h / sys/socket.h / sys/wait.h /
# sys/statvfs.h / dlfcn.h / 两参数 mkdir),另一族是它们的薄适配层 ——
# 适配层自己语法干净,但符号定义在上面那些文件里,拆开就会链接失败,
# 所以必须成组进出。
if(NOT WIN32)
  list(APPEND XMEX_RUNTIME_SOURCES
    runtime/cuda.cpp
    runtime/server.cpp
    runtime/engine.cpp
    runtime/sandbox.cpp
    runtime/sandbox_api.cpp
    runtime/cgroup.cpp
    runtime/cgroup_api.cpp
    runtime/sysmon.cpp
    runtime/sysmon_api.cpp
    runtime/cluster.cpp
    runtime/cluster_api.cpp
    runtime/kv_admin.cpp
    runtime/kv_admin_api.cpp
    runtime/kv_queue_api.cpp
    runtime/plugin_api.cpp
    runtime/lsm_client.cpp
  )
endif()

add_library(xmex_runtime SHARED ${XMEX_RUNTIME_SOURCES})
target_include_directories(xmex_runtime PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
if(WIN32)
  # trace.cpp 的 _WIN32 分支用 winsock(socket/bind/listen/accept/recv/send/
  # closesocket/gethostname)。trace.cpp 里的 #pragma comment(lib, "ws2_32.lib")
  # 只对 MSVC 生效,MinGW 会直接忽略 —— 必须在这里显式链接,
  # 否则与 trace 是否被调用无关,链接期就是一堆 undefined reference。
  target_link_libraries(xmex_runtime PRIVATE ws2_32)
else()
  # Linux 上 GCC 默认隐藏符号，需要显式设置默认可见性以匹配 MinGW 行为
  target_compile_options(xmex_runtime PRIVATE -fvisibility=default)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(SECCOMP REQUIRED libseccomp)
  target_include_directories(xmex_runtime PRIVATE ${SECCOMP_INCLUDE_DIRS})
  find_package(OpenSSL REQUIRED)
  target_link_libraries(xmex_runtime PRIVATE pthread dl /usr/lib/x86_64-linux-gnu/libseccomp.a /usr/lib/x86_64-linux-gnu/libssl.a /usr/lib/x86_64-linux-gnu/libcrypto.a)
endif()

# ---------------------------------------------------------------------------
# 前端(词法 / 语法 / AST)—— 不依赖 LLVM
# ---------------------------------------------------------------------------
# 刻意与 LLVM 完全解耦:前端可以用最朴素的方式构建和单元测试,
# 报错也只可能是语言层面的问题,不会和 JIT/代码生成的坑混在一起。
add_library(xmex_frontend STATIC
  frontend/lexer.cpp
  frontend/ast.cpp
  frontend/parser.cpp
)
target_include_directories(xmex_frontend PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/frontend>
)

add_executable(lex_dump tests/lex_dump.cpp)
target_link_libraries(lex_dump PRIVATE xmex_frontend)

add_executable(ast_dump tests/ast_dump.cpp)
target_link_libraries(ast_dump PRIVATE xmex_frontend)

add_executable(frontend_test tests/frontend_test.cpp)
target_link_libraries(frontend_test PRIVATE xmex_frontend)

# ---------------------------------------------------------------------------
# 代码生成(依赖 LLVM)
# ---------------------------------------------------------------------------
# irgen 与 aot 放同一个库里:两者都直接用 LLVM C++ API,且共享
# kTargetTriple / initializeCodeGenTargets 这些必须成对出现的约定。
add_library(xmex_irgen STATIC
  irgen/abi.cpp
  irgen/irgen.cpp
  aot/compiler.cpp
)
# 公开项目根目录与 include/:irgen 的源文件会 include "frontend/lexer.h"
# 和 "xmex/value.h" 这种路径
target_include_directories(xmex_irgen PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  ${LLVM_INCLUDE_DIRS}
)
target_link_libraries(xmex_irgen PUBLIC xmex_frontend LLVM)

add_executable(xmexc tools/xmexc.cpp)
target_link_libraries(xmexc PRIVATE xmex_irgen)

# ---------------------------------------------------------------------------
# Stage 0 验证
# ---------------------------------------------------------------------------
add_executable(abi_test tests/abi_test.cpp)
target_link_libraries(abi_test PRIVATE xmex_runtime)

# 运行时语义的直接测试(不经过 IRGen)。放在这一层是为了让 e2e 失败时
# 能立刻排除"运行时算错"这一整类原因。
add_executable(runtime_test tests/runtime_test.cpp)
target_link_libraries(runtime_test PRIVATE xmex_runtime)

# GC 专项测试:守恒律、真的回收了、不误回收、环。
add_executable(gc_test tests/gc_test.cpp)
target_link_libraries(gc_test PRIVATE xmex_runtime)

# 调谐循环与副本组测试(2026-10 增补)。
#
# 刻意**不**链 xmex_runtime,而是直接编 kv_admin.cpp:这个测试走
# set_spawn_hook 注入模式,不 fork 真实进程,因此能在任何平台跑 ——
# 包括 Windows,那里没有 fork。这样「观测→判断→动作」这套逻辑
# 不必等到有 Linux CI 才能验证。
#
# kv_admin.cpp 依赖 lsm_client.cpp(注册表持久化),一并编进来。
if(NOT WIN32)
  add_executable(reconcile_test tests/reconcile_test.cpp
                 runtime/kv_admin.cpp runtime/lsm_client.cpp)
  target_include_directories(reconcile_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/runtime)
  target_link_libraries(reconcile_test PRIVATE pthread)
  add_test(NAME reconcile_test COMMAND reconcile_test)
endif()

# 容器节点测试(2026-10 增补)。
#
# 测的是**不需要 Docker daemon 的那部分**:参数校验、资源限额的命令注入
# 防线、节点表一致性、类型混淆拒绝。真正跑容器的路径(pause/crash/重启)
# 要有 Docker 环境才有意义,留给 Linux CI。
#
# 只在 Linux 编:容器节点走 popen 调 docker,依赖 POSIX 那一套。
if(NOT WIN32)
  add_executable(container_test tests/container_test.cpp
                 runtime/kv_admin.cpp runtime/lsm_client.cpp)
  target_include_directories(container_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/runtime)
  target_link_libraries(container_test PRIVATE pthread)
  add_test(NAME container_test COMMAND container_test)
endif()

# 动态库 FFI 测试(2026-10 增补)。
#
# 测一个**真实的**动态库:先编出 ffi_test_dll(.dll/.so),再让 ffi_test
# 加载它、读描述符、按类型调用。这样测的是真链路,不是桩。
#
# 跨平台:Windows 上编 .dll,Linux 上编 .so,产物名不同,测试里按平台猜。
# 刻意不链 xmex_runtime —— ffi.cpp 只依赖 platform.h 的 dl 抽象。
add_library(ffi_test_dll SHARED tests/ffi_test_dll.cpp)
set_target_properties(ffi_test_dll PROPERTIES
  PREFIX ""                       # Windows 上不要 lib 前缀
  OUTPUT_NAME "ffi_test_dll")

add_executable(ffi_test tests/ffi_test.cpp runtime/ffi.cpp)
target_include_directories(ffi_test PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/runtime)
target_link_libraries(ffi_test PRIVATE ffi_test_dll)

# 测试 exe 与 dll 放同一个目录,测试里用相对路径找
add_test(NAME ffi_test COMMAND ffi_test)
set_tests_properties(ffi_test PROPERTIES
  WORKING_DIRECTORY $<TARGET_FILE_DIR:ffi_test>)

# HTTP 客户端测试:在测试进程里起一个最小 HTTP 服务端(裸 socket),
# 然后用 http_get/http_post 真打它。测的是真链路 —— 真 TCP、真 HTTP 报文、
# 真 chunked 解码,不是 mock。链接上 http_api 才能验到语言层边界。
add_executable(http_test tests/http_test.cpp runtime/http.cpp)
target_include_directories(http_test PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/runtime)
# 测试里用了 std::this_thread::sleep_for —— Windows 上需要 winpthread
# (nanosleep64 在那儿,裸 connect 用不到但 std::thread 会链到)。
if(WIN32)
  target_link_libraries(http_test PRIVATE ws2_32 winpthread)
else()
  target_link_libraries(http_test PRIVATE ssl crypto pthread)
endif()
add_test(NAME http_test COMMAND http_test)

# 语言层边界的验证:Xmex 脚本调 http.get 打真实服务。
# 这里必须链 xmex_runtime —— http_api.cpp 用的是运行时的 str_new/dict_*,
# 缺了链接不过。端到端跑的是真链路:C++ 宿主 + 编译后的 .o + 真实 HTTP。
add_executable(http_api_demo examples/http_demo_main.cpp)
target_include_directories(http_api_demo PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

set(HTTP_DEMO_OBJ ${CMAKE_CURRENT_BINARY_DIR}/http_demo.o)
add_custom_command(
  OUTPUT ${HTTP_DEMO_OBJ}
  COMMAND xmexc ${CMAKE_CURRENT_SOURCE_DIR}/examples/http_demo.pys
          -o ${HTTP_DEMO_OBJ} -I http_demo
  DEPENDS xmexc ${CMAKE_CURRENT_SOURCE_DIR}/examples/http_demo.pys
  COMMENT "xmexc examples/http_demo.pys")
add_custom_target(http_demo_obj DEPENDS ${HTTP_DEMO_OBJ})

add_dependencies(http_api_demo http_demo_obj)
target_sources(http_api_demo PRIVATE ${HTTP_DEMO_OBJ})
target_link_libraries(http_api_demo PRIVATE xmex_runtime)

# 演示脚本自带一个最小 HTTP 服务端(用 kv.ffi 加载一个测试用的 .so? 不行 ——
# 那依赖 DLL)。改成让脚本只打一个外部传入的地址,默认打 127.0.0.1:9 让它
# 快速失败,只验证"调用链通、错误可读"。真打业务的例子见 examples/http_demo.pys。
add_test(NAME http_api_demo COMMAND http_api_demo)
set_tests_properties(http_api_demo PROPERTIES
  WORKING_DIRECTORY $<TARGET_FILE_DIR:http_api_demo>)

# ---- acmdog 测试套件与 CI/CD runner(仅 Linux)----
#
# 这些 .pys 都用了 kv.add / kv.status / kv.erase / kv.stop —— 那些符号来自
# runtime/kv_admin_api.cpp,在 if(NOT WIN32) 段里,Windows 上不存在。
# 所以整个块必须放进平台守卫,否则 Windows 构建会在**链接**阶段失败:
# 编译能过(irgen 只是生成对 kv_add_v 的调用),链接器找不到符号才报。
if(NOT WIN32)
# CI/CD runner:用 Xmex 编写的轮询式流水线执行器。
# 结构同 http_api_demo:C++ 宿主 + 编译后的 .o + 运行时。
add_executable(cicd_runner examples/cicd_runner_main.cpp)
target_include_directories(cicd_runner PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

set(CICD_RUNNER_OBJ ${CMAKE_CURRENT_BINARY_DIR}/cicd_runner.o)
add_custom_command(
  OUTPUT ${CICD_RUNNER_OBJ}
  COMMAND xmexc ${CMAKE_CURRENT_SOURCE_DIR}/examples/cicd_runner.pys
          -o ${CICD_RUNNER_OBJ} -I cicd_runner
  DEPENDS xmexc ${CMAKE_CURRENT_SOURCE_DIR}/examples/cicd_runner.pys
  COMMENT "xmexc examples/cicd_runner.pys")
add_custom_target(cicd_runner_obj DEPENDS ${CICD_RUNNER_OBJ})

add_dependencies(cicd_runner cicd_runner_obj)
target_sources(cicd_runner PRIVATE ${CICD_RUNNER_OBJ})
target_link_libraries(cicd_runner PRIVATE xmex_runtime)

# ACMDOG cloud_judge 单元测试的 Xmex 版本。
add_executable(unit_cloud_judge examples/acmdog_gate/unit_cloud_judge_main.cpp)
target_include_directories(unit_cloud_judge PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

set(UNIT_CJ_OBJ ${CMAKE_CURRENT_BINARY_DIR}/unit_cloud_judge.o)
add_custom_command(
  OUTPUT ${UNIT_CJ_OBJ}
  COMMAND xmexc ${CMAKE_CURRENT_SOURCE_DIR}/examples/acmdog_gate/unit_cloud_judge.pys
          -o ${UNIT_CJ_OBJ} -I unit_cloud_judge
  DEPENDS xmexc ${CMAKE_CURRENT_SOURCE_DIR}/examples/acmdog_gate/unit_cloud_judge.pys
  COMMENT "xmexc examples/acmdog_gate/unit_cloud_judge.pys")
add_custom_target(unit_cloud_judge_obj DEPENDS ${UNIT_CJ_OBJ})

add_dependencies(unit_cloud_judge unit_cloud_judge_obj)
target_sources(unit_cloud_judge PRIVATE ${UNIT_CJ_OBJ})
target_link_libraries(unit_cloud_judge PRIVATE xmex_runtime)

# ACMDOG lsm 单元测试的 Xmex 版本。
add_executable(unit_lsm examples/acmdog_gate/unit_lsm_main.cpp)
target_include_directories(unit_lsm PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

set(UNIT_LSM_OBJ ${CMAKE_CURRENT_BINARY_DIR}/unit_lsm.o)
add_custom_command(
  OUTPUT ${UNIT_LSM_OBJ}
  COMMAND xmexc ${CMAKE_CURRENT_SOURCE_DIR}/examples/acmdog_gate/unit_lsm.pys
          -o ${UNIT_LSM_OBJ} -I unit_lsm
  DEPENDS xmexc ${CMAKE_CURRENT_SOURCE_DIR}/examples/acmdog_gate/unit_lsm.pys
  COMMENT "xmexc examples/acmdog_gate/unit_lsm.pys")
add_custom_target(unit_lsm_obj DEPENDS ${UNIT_LSM_OBJ})

add_dependencies(unit_lsm unit_lsm_obj)
target_sources(unit_lsm PRIVATE ${UNIT_LSM_OBJ})
target_link_libraries(unit_lsm PRIVATE xmex_runtime)

# ACMDOG middleware 单元测试的 Xmex 版本。
add_executable(unit_middleware examples/acmdog_gate/unit_middleware_main.cpp)
target_include_directories(unit_middleware PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

set(UNIT_MW_OBJ ${CMAKE_CURRENT_BINARY_DIR}/unit_middleware.o)
add_custom_command(
  OUTPUT ${UNIT_MW_OBJ}
  COMMAND xmexc ${CMAKE_CURRENT_SOURCE_DIR}/examples/acmdog_gate/unit_middleware.pys
          -o ${UNIT_MW_OBJ} -I unit_middleware
  DEPENDS xmexc ${CMAKE_CURRENT_SOURCE_DIR}/examples/acmdog_gate/unit_middleware.pys
  COMMENT "xmexc examples/acmdog_gate/unit_middleware.pys")
add_custom_target(unit_middleware_obj DEPENDS ${UNIT_MW_OBJ})

add_dependencies(unit_middleware unit_middleware_obj)
target_sources(unit_middleware PRIVATE ${UNIT_MW_OBJ})
target_link_libraries(unit_middleware PRIVATE xmex_runtime)

# ACMDOG nginx 单元测试的 Xmex 版本。
add_executable(unit_nginx examples/acmdog_gate/unit_nginx_main.cpp)
target_include_directories(unit_nginx PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

set(UNIT_NG_OBJ ${CMAKE_CURRENT_BINARY_DIR}/unit_nginx.o)
add_custom_command(
  OUTPUT ${UNIT_NG_OBJ}
  COMMAND xmexc ${CMAKE_CURRENT_SOURCE_DIR}/examples/acmdog_gate/unit_nginx.pys
          -o ${UNIT_NG_OBJ} -I unit_nginx
  DEPENDS xmexc ${CMAKE_CURRENT_SOURCE_DIR}/examples/acmdog_gate/unit_nginx.pys
  COMMENT "xmexc examples/acmdog_gate/unit_nginx.pys")
add_custom_target(unit_nginx_obj DEPENDS ${UNIT_NG_OBJ})

add_dependencies(unit_nginx unit_nginx_obj)
target_sources(unit_nginx PRIVATE ${UNIT_NG_OBJ})
target_link_libraries(unit_nginx PRIVATE xmex_runtime)

# ACMDOG 全链路集成测试的 Xmex 版本。
add_executable(integration_full examples/acmdog_gate/integration_full_main.cpp)
target_include_directories(integration_full PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

set(INTEGRATION_OBJ ${CMAKE_CURRENT_BINARY_DIR}/integration_full.o)
add_custom_command(
  OUTPUT ${INTEGRATION_OBJ}
  COMMAND xmexc ${CMAKE_CURRENT_SOURCE_DIR}/examples/acmdog_gate/integration_full.pys
          -o ${INTEGRATION_OBJ} -I integration_full
  DEPENDS xmexc ${CMAKE_CURRENT_SOURCE_DIR}/examples/acmdog_gate/integration_full.pys
  COMMENT "xmexc examples/acmdog_gate/integration_full.pys")
add_custom_target(integration_full_obj DEPENDS ${INTEGRATION_OBJ})

add_dependencies(integration_full integration_full_obj)
target_sources(integration_full PRIVATE ${INTEGRATION_OBJ})
target_link_libraries(integration_full PRIVATE xmex_runtime)

endif()

# FFI 隔离模式的 agent:独立可执行文件,加载 DLL 并处理宿主的管道请求。
#
# 刻意做成独立 exe 而不是直接 fork 宿主:
#   1. 子进程是干净的,只加载目标 DLL —— 不会把宿主已加载的运行时、
#      已注册的 GC 根、已打开的套接字都带过去
#   2. DLL 崩了时,退出码干净地反映在 agent 的退出状态上
#   3. 将来做远程 agent 时,同一个 exe 换个连接方式即可
#
# 依赖 ffi.cpp(进程内 FFI)+ffi_worker.cpp(请求循环),不链 xmex_runtime ——
# agent 不需要语言运行时,只要能调 DLL 就行。
add_executable(xmex_ffi_agent tools/ffi_agent.cpp
               runtime/ffi_worker.cpp runtime/ffi.cpp)
target_include_directories(xmex_ffi_agent PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/runtime)

# agent 必须和 xmex_runtime 在同一个目录 —— 宿主靠"同目录"推断它的位置
# (见 runtime/ffi_api.cpp 的 agent_path())。两个目标的输出目录都是
# ${CMAKE_BINARY_DIR}/bin,所以天然满足。

# FFI 的端到端演示:C++ 宿主 + Xmex 脚本实际加载动态库并调用。
#
# 这是唯一能验证「Xmex 源码 → 编译 → 链接 → 加载 DLL → 调用」整条链路的
# 用例。ffi_test 只测 C++ 层的 ffi.cpp,测不到 irgen 的映射与 ABI 约定。
add_executable(ffi_demo examples/ffi_main.cpp)
target_include_directories(ffi_demo PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

# 先把 .pys 编译成 .o,再和宿主一起链接
set(FFI_DEMO_OBJ ${CMAKE_CURRENT_BINARY_DIR}/ffi_demo.o)
add_custom_command(
  OUTPUT ${FFI_DEMO_OBJ}
  COMMAND xmexc ${CMAKE_CURRENT_SOURCE_DIR}/examples/ffi_demo.pys
          -o ${FFI_DEMO_OBJ} -I ffi_demo
  DEPENDS xmexc ${CMAKE_CURRENT_SOURCE_DIR}/examples/ffi_demo.pys
  COMMENT "xmexc examples/ffi_demo.pys")
add_custom_target(ffi_demo_obj DEPENDS ${FFI_DEMO_OBJ})

add_dependencies(ffi_demo ffi_demo_obj)
target_sources(ffi_demo PRIVATE ${FFI_DEMO_OBJ})
target_link_libraries(ffi_demo PRIVATE xmex_runtime ffi_test_dll)

add_test(NAME ffi_demo COMMAND ffi_demo)
set_tests_properties(ffi_demo PROPERTIES
  WORKING_DIRECTORY $<TARGET_FILE_DIR:ffi_demo>)

# FFI 隔离模式的端到端演示。
# 关键验证点:dll_crash 让 DLL 段错误,宿主必须活着并继续往下跑 ——
# 这正是隔离的意义,也是进程内模式做不到的。
add_executable(ffi_isolated_demo examples/ffi_isolated_main.cpp)
target_include_directories(ffi_isolated_demo PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

set(FFI_ISOLATED_OBJ ${CMAKE_CURRENT_BINARY_DIR}/ffi_isolated_demo.o)
add_custom_command(
  OUTPUT ${FFI_ISOLATED_OBJ}
  COMMAND xmexc ${CMAKE_CURRENT_SOURCE_DIR}/examples/ffi_isolated_demo.pys
          -o ${FFI_ISOLATED_OBJ} -I ffi_isolated_demo
  DEPENDS xmexc ${CMAKE_CURRENT_SOURCE_DIR}/examples/ffi_isolated_demo.pys
  COMMENT "xmexc examples/ffi_isolated_demo.pys")
add_custom_target(ffi_isolated_demo_obj DEPENDS ${FFI_ISOLATED_OBJ})

add_dependencies(ffi_isolated_demo ffi_isolated_demo_obj xmex_ffi_agent)
target_sources(ffi_isolated_demo PRIVATE ${FFI_ISOLATED_OBJ})
target_link_libraries(ffi_isolated_demo PRIVATE xmex_runtime ffi_test_dll)

add_test(NAME ffi_isolated_demo COMMAND ffi_isolated_demo)
set_tests_properties(ffi_isolated_demo PROPERTIES
  WORKING_DIRECTORY $<TARGET_FILE_DIR:ffi_isolated_demo>)

add_executable(jit_spike tests/jit_spike.cpp)
target_include_directories(jit_spike PRIVATE ${LLVM_INCLUDE_DIRS})
target_link_libraries(jit_spike PRIVATE LLVM xmex_runtime)

# ---------------------------------------------------------------------------
# 诊断工具:确认 LLVM 目标注册表状态(非测试,出问题时手动跑)
# ---------------------------------------------------------------------------
add_executable(target_probe tests/target_probe.cpp)
target_include_directories(target_probe PRIVATE ${LLVM_INCLUDE_DIRS})
target_link_libraries(target_probe PRIVATE LLVM)

# ---------------------------------------------------------------------------
# 测试
# ---------------------------------------------------------------------------
enable_testing()
add_test(NAME abi_test COMMAND abi_test)
# 显式把运行时库的全路径传进去:JIT 要按名字加载它解析 py_* 符号,
# 不传的话就得靠默认文件名猜(Windows 是 .dll、Linux 是 .so)。
add_test(NAME jit_spike COMMAND jit_spike $<TARGET_FILE:xmex_runtime>)
add_test(NAME frontend_test COMMAND frontend_test)
add_test(NAME runtime_test COMMAND runtime_test)
add_test(NAME gc_test COMMAND gc_test)

# examples/ 下的每个示例都必须能被成功解析(退出码 0)。
# 这是最轻量的检查:源码进、AST 出,覆盖前端全链路。
foreach(example range_print fib containers)
  add_test(NAME parse_${example}
           COMMAND ast_dump ${CMAKE_CURRENT_SOURCE_DIR}/examples/${example}.pys)
endforeach()

# ---------------------------------------------------------------------------
# 端到端测试:.pys -> 目标文件 -> 与 C++ 链接 -> 运行 -> 比对 stdout
# ---------------------------------------------------------------------------
# 每个用例配一个最小的 C++ 驱动,它只做一件事:调用 xmexc 编译出来的入口。
# 这一层验证的正是项目的核心诉求 —— "在编译期集成进 C++"。
# 只做 AST 层面的检查是不够的:之前 print(a, b) 的 GEP 语义写错时,
# IR 验证通过、编译成功、链接成功,只有真正运行才暴露出来。
set(E2E_CASES
  arith_ops
  containers
  dicts
  fib
  for_step
  gc_stress
  if_else
  inplace_add
  input_types
  lists
  logic
  long_loop
  loop_sum
  multiassign
  range_print
  slices
  strings
  unpack_call
  while_ctl
)

# 手写 C++ 链接 xmexc 产物的集成测试。tests/linktest/ 一直都在,但之前只有
# 手工步骤;接进 CTest 之后,项目的核心诉求 —— "把 Xmex 编译进 C++ 程序" ——
# 每次构建都会被真正验证一次。
set(LINKTEST_OBJ ${CMAKE_CURRENT_BINARY_DIR}/linktest/arith.o)
add_custom_command(
  OUTPUT ${LINKTEST_OBJ}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/linktest
  COMMAND xmexc ${CMAKE_CURRENT_SOURCE_DIR}/tests/linktest/arith.pys
          -o ${LINKTEST_OBJ}
  DEPENDS xmexc ${CMAKE_CURRENT_SOURCE_DIR}/tests/linktest/arith.pys
  COMMENT "xmexc linktest/arith.pys")

add_executable(linktest tests/linktest/main.cpp ${LINKTEST_OBJ})
target_link_libraries(linktest PRIVATE xmex_runtime)

add_test(NAME linktest
         COMMAND ${CMAKE_COMMAND}
                 -DEXE=$<TARGET_FILE:linktest>
                 -DEXPECTED=${CMAKE_CURRENT_SOURCE_DIR}/tests/linktest/arith.expected
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/compare_output.cmake)

# ---------------------------------------------------------------------------
# 混合语言示例:C++ 宿主 + Xmex 业务逻辑
# ---------------------------------------------------------------------------
# examples/mixed/ 是给人看的一个完整例子(C++ 造数据、Xmex 算、C++ 收结果),
# 文档在同目录的 README.md。这里把它接进构建,保证它随时可编译可链接。
#
# 只断言"退出码为 0",不比对 stdout:它的输出是给人看的中文,
# 而 tests/compare_output.cmake 在 Windows 上只能比 ASCII。
set(MIXED_OBJ ${CMAKE_CURRENT_BINARY_DIR}/mixed/scores.o)
add_custom_command(
  OUTPUT ${MIXED_OBJ}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/mixed
  COMMAND xmexc ${CMAKE_CURRENT_SOURCE_DIR}/examples/mixed/scores.pys
          -o ${MIXED_OBJ}
  DEPENDS xmexc ${CMAKE_CURRENT_SOURCE_DIR}/examples/mixed/scores.pys
  COMMENT "xmexc mixed/scores.pys")

add_executable(mixed_demo examples/mixed/main.cpp ${MIXED_OBJ})
target_link_libraries(mixed_demo PRIVATE xmex_runtime)

add_test(NAME mixed_demo COMMAND mixed_demo)

foreach(t ${E2E_CASES})
  set(pys ${CMAKE_CURRENT_SOURCE_DIR}/tests/e2e/${t}.pys)
  set(exp ${CMAKE_CURRENT_SOURCE_DIR}/tests/e2e/${t}.expected)
  set(inp ${CMAKE_CURRENT_SOURCE_DIR}/tests/e2e/${t}.in)
  set(obj ${CMAKE_CURRENT_BINARY_DIR}/e2e/${t}.o)
  set(drv ${CMAKE_CURRENT_BINARY_DIR}/e2e/${t}_main.cpp)

  # 驱动源码:唯一的职责就是把编译产物调起来
  file(WRITE ${drv}
    "extern \"C\" void xmex_${t}_main();\n"
    "int main() { xmex_${t}_main(); return 0; }\n")

  add_custom_command(
    OUTPUT ${obj}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/e2e
    COMMAND xmexc ${pys} -o ${obj}
    DEPENDS xmexc ${pys}
    COMMENT "xmexc ${t}.pys")

  add_executable(e2e_${t} ${drv} ${obj})
  target_link_libraries(e2e_${t} PRIVATE xmex_runtime)

  # 有同名 .in 文件时,把它作为 stdin 喂给程序(测 input(...) 的用例)
  set(inputArg)
  if(EXISTS ${inp})
    list(APPEND inputArg -DINPUT=${inp})
  endif()

  add_test(NAME e2e_${t}
           COMMAND ${CMAKE_COMMAND}
                   -DEXE=$<TARGET_FILE:e2e_${t}>
                   -DEXPECTED=${exp}
                   ${inputArg}
                   -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/compare_output.cmake)

  # 同一份程序再跑一轮"每次分配都回收"。默认配置下这些用例只分配几 KB,
  # 离任何回收阈值都差着数量级 —— 也就是说 GC 的回收路径**根本没被走到**,
  # 全绿说明不了任何问题。加上这一轮之后,丢根与误回收会立刻暴露,
  # 而输出必须与默认配置逐字节相同(这本身就证明"回收不影响可观测行为")。
  add_test(NAME e2e_${t}_gc
           COMMAND ${CMAKE_COMMAND}
                   -DEXE=$<TARGET_FILE:e2e_${t}>
                   -DEXPECTED=${exp}
                   ${inputArg}
                   -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/compare_output.cmake)
  set_tests_properties(e2e_${t}_gc PROPERTIES
                       ENVIRONMENT "XMEX_GC_STRESS=1")
endforeach()
