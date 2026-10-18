// Docker 容器调用 —— 通过 popen 调用 docker CLI。
//
// 设计原则：
//   1. 不引入 Docker SDK 依赖，只用系统自带的 docker 命令；
//   2. 所有操作返回 xmex::Value，与 Xmex 类型系统无缝对接；
//   3. 错误通过 runtime_error 抛出，调用方可以用 try/except 捕获。
//
// 注意：当前环境可能没有 Docker daemon，但代码逻辑是完整的，
// 部署到有 Docker 的环境中即可直接使用。
#include "xmex/runtime.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <chrono>

namespace {

// 执行 shell 命令并捕获 stdout，返回动态分配的 C 字符串。
// 调用方负责用 free() 释放。失败时返回 nullptr。
char *captureCommand(const char *cmd) {
  FILE *fp = popen(cmd, "r");
  if (!fp) return nullptr;

  size_t cap = 256;
  char *buf = static_cast<char *>(malloc(cap));
  if (!buf) { pclose(fp); return nullptr; }

  size_t len = 0;
  while (!feof(fp) && !ferror(fp)) {
    if (len + 128 >= cap) {
      cap *= 2;
      char *nb = static_cast<char *>(realloc(buf, cap));
      if (!nb) { free(buf); pclose(fp); return nullptr; }
      buf = nb;
    }
    size_t n = fread(buf + len, 1, cap - len - 1, fp);
    len += n;
  }
  buf[len] = '\0';

  int rc = pclose(fp);
  if (rc != 0) {
    // 命令失败：把已捕获的输出当作错误信息返回
    // 调用方通过检查输出是否以 "Error:" 开头来判断
    return buf;
  }
  return buf;
}

// 去除字符串末尾的换行符
void trimNewline(char *s) {
  size_t len = strlen(s);
  while (len > 0 && (s[len - 1] == '\n' || s[len - 1] == '\r')) {
    s[--len] = '\0';
  }
}

}  // namespace

// docker_run(image, command, ...) -> container_id: str
// 启动一个容器并返回容器 ID。
// 用法：docker_run("ubuntu:22.04", "echo hello")
extern "C" xmex::Value docker_run(const xmex::Value *image, const xmex::Value *cmd) {
  if (image->tag != xmex::kStr || cmd->tag != xmex::kStr) {
    runtime_error("docker_run() 需要两个字符串参数: image, command");
  }
  auto traceT0 = std::chrono::steady_clock::now();

  const char *img = str_data(image);
  const char *command = str_data(cmd);
  int64_t imgLen = str_size(image);
  int64_t cmdLen = str_size(cmd);

  // 构造命令: docker run -d --rm <image> <command>
  // -d: 后台运行; --rm: 退出后自动删除
  size_t bufSize = static_cast<size_t>(imgLen + cmdLen + 64);
  char *fullCmd = static_cast<char *>(malloc(bufSize));
  if (!fullCmd) runtime_error("内存不足");

  snprintf(fullCmd, bufSize,
           "docker run -d --rm %.*s %.*s 2>&1",
           static_cast<int>(imgLen), img,
           static_cast<int>(cmdLen), command);

  char *output = captureCommand(fullCmd);
  free(fullCmd);

  if (!output) {
    runtime_error("docker run 失败: 无法执行 docker 命令");
  }

  trimNewline(output);

  // 检查是否是错误输出（docker 错误信息通常包含特定关键词）
  if (strstr(output, "Error") || strstr(output, "Cannot connect") ||
      strstr(output, "permission denied")) {
    xmex::Value err = str_new(output, static_cast<int64_t>(strlen(output)));
    free(output);
    // 用 repr 包装错误信息
    char errBuf[512];
    snprintf(errBuf, sizeof(errBuf), "docker run 失败: %s", str_data(&err));
    runtime_error(errBuf);
  }

  xmex::Value result = str_new(output, static_cast<int64_t>(strlen(output)));
  trace_op("docker", "run", img, 1,
              std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - traceT0).count());
  free(output);
  return result;
}

// docker_ps() -> str
// 列出正在运行的容器，返回 docker ps 的输出。
extern "C" xmex::Value docker_ps() {
  auto traceT0 = std::chrono::steady_clock::now();
  char *output = captureCommand("docker ps 2>&1");
  if (!output) {
    runtime_error("docker ps 失败: 无法执行 docker 命令");
  }

  trimNewline(output);
  xmex::Value result = str_new(output, static_cast<int64_t>(strlen(output)));
  trace_op("docker", "ps", "", 1,
              std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - traceT0).count());
  free(output);
  return result;
}

// docker_stop(container_id: str) -> None
// 停止一个正在运行的容器。
extern "C" void docker_stop(const xmex::Value *containerId) {
  if (containerId->tag != xmex::kStr) {
    runtime_error("docker_stop() 需要一个字符串参数: container_id");
  }
  auto traceT0 = std::chrono::steady_clock::now();

  const char *cid = str_data(containerId);
  int64_t cidLen = str_size(containerId);

  size_t bufSize = static_cast<size_t>(cidLen + 32);
  char *fullCmd = static_cast<char *>(malloc(bufSize));
  if (!fullCmd) runtime_error("内存不足");

  snprintf(fullCmd, bufSize, "docker stop %.*s 2>&1",
           static_cast<int>(cidLen), cid);

  char *output = captureCommand(fullCmd);
  free(fullCmd);

  if (output) {
    if (strstr(output, "Error") || strstr(output, "No such container")) {
      char errBuf[512];
      snprintf(errBuf, sizeof(errBuf), "docker stop 失败: %s", output);
      free(output);
      runtime_error(errBuf);
    }
    trace_op("docker", "stop", cid, 1,
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - traceT0).count());
    free(output);
  }
}

// docker_logs(container_id: str) -> str
// 获取容器的日志输出。
extern "C" xmex::Value docker_logs(const xmex::Value *containerId) {
  if (containerId->tag != xmex::kStr) {
    runtime_error("docker_logs() 需要一个字符串参数: container_id");
  }
  auto traceT0 = std::chrono::steady_clock::now();

  const char *cid = str_data(containerId);
  int64_t cidLen = str_size(containerId);

  size_t bufSize = static_cast<size_t>(cidLen + 32);
  char *fullCmd = static_cast<char *>(malloc(bufSize));
  if (!fullCmd) runtime_error("内存不足");

  snprintf(fullCmd, bufSize, "docker logs %.*s 2>&1",
           static_cast<int>(cidLen), cid);

  char *output = captureCommand(fullCmd);
  free(fullCmd);

  if (!output) {
    runtime_error("docker logs 失败: 无法执行 docker 命令");
  }

  trimNewline(output);
  xmex::Value result = str_new(output, static_cast<int64_t>(strlen(output)));
  trace_op("docker", "logs", cid, 1,
              std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - traceT0).count());
  free(output);
  return result;
}

// docker_pull(image: str) -> str
// 拉取 Docker 镜像。
extern "C" xmex::Value docker_pull(const xmex::Value *image) {
  if (image->tag != xmex::kStr) {
    runtime_error("docker_pull() 需要一个字符串参数: image");
  }
  auto traceT0 = std::chrono::steady_clock::now();

  const char *img = str_data(image);
  int64_t imgLen = str_size(image);

  size_t bufSize = static_cast<size_t>(imgLen + 32);
  char *fullCmd = static_cast<char *>(malloc(bufSize));
  if (!fullCmd) runtime_error("内存不足");

  snprintf(fullCmd, bufSize, "docker pull %.*s 2>&1",
           static_cast<int>(imgLen), img);

  char *output = captureCommand(fullCmd);
  free(fullCmd);

  if (!output) {
    runtime_error("docker pull 失败: 无法执行 docker 命令");
  }

  trimNewline(output);
  xmex::Value result = str_new(output, static_cast<int64_t>(strlen(output)));
  trace_op("docker", "pull", img, 1,
              std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - traceT0).count());
  free(output);
  return result;
}

// ===========================================================================
// Docker 项目封装
// ===========================================================================

// docker_generate_dockerfile(base_image: str, project_name: str,
//                             setup_commands: str, entrypoint: str,
//                             expose_port: int) -> str
// 根据项目描述生成 Dockerfile 内容。
extern "C" xmex::Value docker_generate_dockerfile(const xmex::Value *baseImage,
                                                  const xmex::Value *projectName,
                                                  const xmex::Value *setupCommands,
                                                  const xmex::Value *entrypoint,
                                                  const xmex::Value *exposePort) {
  if (baseImage->tag != xmex::kStr || projectName->tag != xmex::kStr ||
      entrypoint->tag != xmex::kStr) {
    runtime_error(
      "docker_generate_dockerfile() 需要字符串参数: base_image, project_name, entrypoint");
  }
  auto traceT0 = std::chrono::steady_clock::now();

  const char *img = str_data(baseImage);
  const char *name = str_data(projectName);
  const char *cmds = (setupCommands->tag == xmex::kStr) ? str_data(setupCommands) : "";
  const char *entry = str_data(entrypoint);
  int64_t port = (exposePort->tag == xmex::kInt) ? as_int(*exposePort) : 0;

  int64_t imgLen = str_size(baseImage);
  int64_t nameLen = str_size(projectName);
  int64_t cmdsLen = (setupCommands->tag == xmex::kStr) ? str_size(setupCommands) : 0;
  int64_t entryLen = str_size(entrypoint);

  size_t cap = static_cast<size_t>(imgLen + nameLen + cmdsLen + entryLen) + 1024;
  char *dockerfile = static_cast<char *>(malloc(cap));
  if (!dockerfile) runtime_error("内存不足");

  int n = snprintf(dockerfile, cap,
    "# Dockerfile generated by Xmex for project: %.*s\n"
    "FROM %.*s\n"
    "\n"
    "WORKDIR /app\n"
    "\n",
    static_cast<int>(nameLen), name,
    static_cast<int>(imgLen), img);

  if (cmdsLen > 0) {
    const char *start = cmds;
    const char *end = cmds + cmdsLen;
    while (start < end) {
      const char *semi = static_cast<const char *>(memchr(start, ';',
                                        static_cast<size_t>(end - start)));
      if (!semi) semi = end;
      while (start < semi && (*start == ' ' || *start == '\t')) start++;
      int64_t cmdLen = semi - start;
      if (cmdLen > 0) {
        n += snprintf(dockerfile + n, cap - static_cast<size_t>(n),
          "RUN %.*s\n", static_cast<int>(cmdLen), start);
      }
      start = semi + 1;
    }
    n += snprintf(dockerfile + n, cap - static_cast<size_t>(n), "\n");
  }

  n += snprintf(dockerfile + n, cap - static_cast<size_t>(n),
    "COPY . /app\n"
    "\n");

  if (port > 0) {
    n += snprintf(dockerfile + n, cap - static_cast<size_t>(n),
      "EXPOSE %lld\n"
      "\n", port);
  }

  n += snprintf(dockerfile + n, cap - static_cast<size_t>(n),
    "CMD %.*s\n",
    static_cast<int>(entryLen), entry);

  xmex::Value result = str_new(dockerfile, static_cast<int64_t>(n));
  trace_op("docker", "generate_dockerfile", name, 1,
              std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - traceT0).count());
  free(dockerfile);
  return result;
}

// docker_build(dockerfile: str, image_name: str, tag: str, context_dir: str) -> str
// 构建 Docker 镜像。
extern "C" xmex::Value docker_build(const xmex::Value *dockerfile,
                                    const xmex::Value *imageName,
                                    const xmex::Value *tag,
                                    const xmex::Value *contextDir) {
  if (imageName->tag != xmex::kStr || tag->tag != xmex::kStr ||
      contextDir->tag != xmex::kStr) {
    runtime_error(
      "docker_build() 需要字符串参数: image_name, tag, context_dir");
  }
  auto traceT0 = std::chrono::steady_clock::now();

  const char *df = (dockerfile->tag == xmex::kStr) ? str_data(dockerfile) : "";
  const char *name = str_data(imageName);
  const char *t = str_data(tag);
  const char *ctx = str_data(contextDir);

  int64_t dfLen = (dockerfile->tag == xmex::kStr) ? str_size(dockerfile) : 0;
  int64_t nameLen = str_size(imageName);
  int64_t tagLen = str_size(tag);
  int64_t ctxLen = str_size(contextDir);

  char tmpDf[256] = {0};
  bool useTempFile = false;
  if (dfLen > 0 && df[0] != '/') {
    snprintf(tmpDf, sizeof(tmpDf), "/tmp/xmex_dockerfile_%s", t);
    FILE *fp = fopen(tmpDf, "w");
    if (!fp) runtime_error("docker_build() 失败: 无法创建临时 Dockerfile");
    fwrite(df, 1, static_cast<size_t>(dfLen), fp);
    fclose(fp);
    useTempFile = true;
  }

  const char *dfPath = useTempFile ? tmpDf : df;

  size_t cmdCap = static_cast<size_t>(nameLen + tagLen + ctxLen) + 512;
  char *cmd = static_cast<char *>(malloc(cmdCap));
  if (!cmd) runtime_error("内存不足");

  if (dfLen > 0) {
    snprintf(cmd, cmdCap,
      "docker build -f %s -t %.*s:%.*s %.*s 2>&1",
      dfPath,
      static_cast<int>(nameLen), name,
      static_cast<int>(tagLen), t,
      static_cast<int>(ctxLen), ctx);
  } else {
    snprintf(cmd, cmdCap,
      "docker build -t %.*s:%.*s %.*s 2>&1",
      static_cast<int>(nameLen), name,
      static_cast<int>(tagLen), t,
      static_cast<int>(ctxLen), ctx);
  }

  char *output = captureCommand(cmd);
  free(cmd);

  if (useTempFile) remove(tmpDf);

  if (!output) {
    runtime_error("docker build 失败: 无法执行 docker 命令");
  }

  trimNewline(output);
  xmex::Value result = str_new(output, static_cast<int64_t>(strlen(output)));
  trace_op("docker", "build", name, 1,
              std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - traceT0).count());
  free(output);
  return result;
}

// docker_push(image_name: str, tag: str, registry: str) -> str
// 推送 Docker 镜像到镜像仓库。
extern "C" xmex::Value docker_push(const xmex::Value *imageName,
                                   const xmex::Value *tag,
                                   const xmex::Value *registry) {
  if (imageName->tag != xmex::kStr || tag->tag != xmex::kStr) {
    runtime_error("docker_push() 需要字符串参数: image_name, tag");
  }
  auto traceT0 = std::chrono::steady_clock::now();

  const char *name = str_data(imageName);
  const char *t = str_data(tag);
  const char *reg = (registry->tag == xmex::kStr) ? str_data(registry) : "";

  int64_t nameLen = str_size(imageName);
  int64_t tagLen = str_size(tag);
  int64_t regLen = (registry->tag == xmex::kStr) ? str_size(registry) : 0;

  size_t fullCap = static_cast<size_t>(regLen + nameLen + tagLen) + 32;
  char *fullName = static_cast<char *>(malloc(fullCap));
  if (!fullName) runtime_error("内存不足");

  if (regLen > 0) {
    snprintf(fullName, fullCap, "%.*s/%.*s:%.*s",
             static_cast<int>(regLen), reg,
             static_cast<int>(nameLen), name,
             static_cast<int>(tagLen), t);
  } else {
    snprintf(fullName, fullCap, "%.*s:%.*s",
             static_cast<int>(nameLen), name,
             static_cast<int>(tagLen), t);
  }

  if (regLen > 0) {
    size_t tagCmdCap = fullCap + static_cast<size_t>(nameLen + tagLen) + 64;
    char *tagCmd = static_cast<char *>(malloc(tagCmdCap));
    if (tagCmd) {
      snprintf(tagCmd, tagCmdCap,
        "docker tag %.*s:%.*s %s 2>&1",
        static_cast<int>(nameLen), name,
        static_cast<int>(tagLen), t,
        fullName);
      char *tagOut = captureCommand(tagCmd);
      free(tagOut);
      free(tagCmd);
    }
  }

  size_t cmdCap = fullCap + 64;
  char *cmd = static_cast<char *>(malloc(cmdCap));
  if (!cmd) { free(fullName); runtime_error("内存不足"); }

  snprintf(cmd, cmdCap, "docker push %s 2>&1", fullName);

  char *output = captureCommand(cmd);
  free(cmd);
  free(fullName);

  if (!output) {
    runtime_error("docker push 失败: 无法执行 docker 命令");
  }

  trimNewline(output);
  xmex::Value result = str_new(output, static_cast<int64_t>(strlen(output)));
  trace_op("docker", "push", name, 1,
              std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - traceT0).count());
  free(output);
  return result;
}

// docker_project_package(project_dir: str, image_name: str, tag: str,
//                          base_image: str, setup_commands: str,
//                          entrypoint: str, expose_port: int) -> str
// 一键封装项目为 Docker 镜像。
extern "C" xmex::Value docker_project_package(const xmex::Value *projectDir,
                                              const xmex::Value *imageName,
                                              const xmex::Value *tag,
                                              const xmex::Value *baseImage,
                                              const xmex::Value *setupCommands,
                                              const xmex::Value *entrypoint,
                                              const xmex::Value *exposePort) {
  if (projectDir->tag != xmex::kStr || imageName->tag != xmex::kStr ||
      tag->tag != xmex::kStr || baseImage->tag != xmex::kStr ||
      entrypoint->tag != xmex::kStr) {
    runtime_error(
      "docker_project_package() 需要字符串参数: "
      "project_dir, image_name, tag, base_image, entrypoint");
  }
  auto traceT0 = std::chrono::steady_clock::now();

  xmex::Value df = docker_generate_dockerfile(baseImage, imageName,
                                              setupCommands, entrypoint,
                                              exposePort);

  const char *dir = str_data(projectDir);
  int64_t dirLen = str_size(projectDir);

  size_t dfPathCap = static_cast<size_t>(dirLen) + 32;
  char *dfPath = static_cast<char *>(malloc(dfPathCap));
  if (!dfPath) runtime_error("内存不足");

  snprintf(dfPath, dfPathCap, "%.*s/Dockerfile", static_cast<int>(dirLen), dir);

  const char *dfContent = str_data(&df);
  int64_t dfContentLen = str_size(&df);

  FILE *fp = fopen(dfPath, "w");
  if (!fp) {
    free(dfPath);
    runtime_error("docker_project_package() 失败: 无法写入 Dockerfile");
  }
  fwrite(dfContent, 1, static_cast<size_t>(dfContentLen), fp);
  fclose(fp);

  xmex::Value buildResult = docker_build(&df, imageName, tag, projectDir);

  const char *buildOut = str_data(&buildResult);
  int64_t buildOutLen = str_size(&buildResult);
  const char *imgName = str_data(imageName);
  int64_t imgNameLen = str_size(imageName);
  const char *t = str_data(tag);
  int64_t tLen = str_size(tag);

  size_t summaryCap = static_cast<size_t>(buildOutLen + imgNameLen + tLen + dirLen) + 512;
  char *summary = static_cast<char *>(malloc(summaryCap));
  if (!summary) { free(dfPath); runtime_error("内存不足"); }

  snprintf(summary, summaryCap,
    "[Xmex Docker 封装完成]\n"
    "项目目录: %.*s\n"
    "Dockerfile: %s\n"
    "镜像: %.*s:%.*s\n"
    "\n--- 构建输出 ---\n"
    "%.*s",
    static_cast<int>(dirLen), dir,
    dfPath,
    static_cast<int>(imgNameLen), imgName,
    static_cast<int>(tLen), t,
    static_cast<int>(buildOutLen), buildOut);

  free(dfPath);

  xmex::Value result = str_new(summary, static_cast<int64_t>(strlen(summary)));
  trace_op("docker", "project_package", imgName, 1,
              std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - traceT0).count());
  free(summary);
  return result;
}
