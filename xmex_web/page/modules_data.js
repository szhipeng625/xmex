const MODULES = {
  docker: { desc: "Docker 容器管理", fns: [
    ["run(image, cmd)", "运行容器；image 字符串(镜像名)，cmd 字符串(启动命令)"],["ps()", "列出全部容器"],["stop(containerId)", "停止指定容器；containerId 字符串"],["logs(containerId)", "查看容器日志；containerId 字符串"],["pull(image)", "拉取镜像；image 字符串"],["generate_dockerfile(baseImage, projectName, setupCommands, entrypoint, exposePort)", "生成 Dockerfile；baseImage/projectName/entrypoint 字符串，setupCommands 字符串(安装命令，多行)，exposePort 整数(端口)"],["build(dockerfile, imageName, tag, contextDir)", "构建镜像；四个参数均为字符串(dockerfile 路径、镜像名、标签、上下文目录)"],["push(imageName, tag, registry)", "推送镜像；三个参数均为字符串"],["project_package(projectDir, imageName, tag, baseImage, setupCommands, entrypoint, exposePort)", "项目整体打包；exposePort 整数，其余字符串"]
  ]},
  llm: { desc: "大语言模型与微调", fns: [
    ["chat(prompt, systemPrompt, model, apiKey, endpoint)", "对话调用；五个参数均为字符串，endpoint 形如 http://host:port/v1"],["create_network(name, layers)", "创建网络；name 字符串，layers 列表(每层为字典定义)"],["add_layer(network, layer)", "添加层；network 字符串(网络句柄)，layer 字典"],["remove_layer(network, index)", "删除层；index 整数"],["network_summary(network)", "网络结构摘要；network 字符串"],["create_dataset(examples, systemPrompt)", "创建数据集；examples 列表，systemPrompt 字符串"],["upload_file(data, filename, apiKey, endpoint)", "上传文件；data 字符串(文件内容)，其余字符串"],["create_finetune(fileId, model, suffix, apiKey, endpoint)", "创建微调任务；五个参数均为字符串"],["finetune_status(jobId, apiKey, endpoint)", "查询微调状态；均为字符串"],["list_finetunes(apiKey, endpoint)", "列出微调任务；均为字符串"]
  ]},
  cuda: { desc: "CUDA GPU 计算", fns: [
    ["info()", "查询 GPU 信息"],["compile_ptx(kernelCode, kernelName)", "编译内核；两个参数均为字符串"],["launch_kernel(ptxCode, kernelName, gridX, gridY, gridZ, blockX, blockY, blockZ)", "启动内核；前两个字符串，后六个为整数(网格/线程块三维尺寸，常用 1 起)"],["alloc(size)", "分配显存；size 整数(字节)"],["free(handle)", "释放显存；handle 整数(句柄)"],["memcpy_to_device(handle, data)", "复制到设备；handle 整数，data 字符串或列表"],["memcpy_from_device(handle, size)", "复制回主机；handle 整数，size 整数(字节)"]
  ]},
  fusion: { desc: "算子融合优化", fns: [
    ["create_graph(name)", "创建计算图；name 字符串"],["add_op(graph, opType, opName, params)", "添加算子；前三个字符串，params 字典或字符串"],["apply_rules(graph)", "应用融合规则；graph 字符串"],["generate_kernel(fusedOp, config)", "生成内核；两个参数均为字符串"],["autotune(opType, M, N, K)", "自动调优；opType 字符串，M/N/K 整数(矩阵维度)"],["compile_and_run(kernelCode, kernelName, M, N, K)", "编译运行；前两个字符串，后三个整数"],["benchmark(kernelCode, kernelName, M, N, K, iterations)", "基准测试；前两个字符串，后四个整数(iterations 为迭代次数)"]
  ]},
  inference: { desc: "模型推理", fns: [
    ["load_model(modelPath, configPath)", "加载模型；两个参数均为字符串(文件路径)"],["generate(prompt, maxTokens, temperature, topP, topK)", "生成文本；prompt 字符串，maxTokens 整数(默认 256)，temperature 浮点(默认 0.7)，topP 浮点(默认 0.9)，topK 整数(默认 50)"],["kv_cache_stats()", "KV 缓存统计"],["flash_attention_kernel()", "FlashAttention 内核源码"],["multi_gpu_info()", "多 GPU 信息"],["profile()", "性能分析"],["unload()", "卸载模型"]
  ]},
  tokenizer: { desc: "分词器", fns: [
    ["load(path)", "加载分词器；path 字符串"],["encode(text)", "编码为 token；text 字符串，返回整数列表"],["decode(tokenIds)", "解码为文本；tokenIds 整数列表"],["info()", "分词器信息"]
  ]},
  server: { desc: "HTTP 服务", fns: [
    ["start(port)", "启动服务；port 整数(端口号，默认 8080)"],["stop()", "停止服务"],["status()", "服务状态"],["config(secret, node_id)", "集群配置；两个参数均为字符串"],["tls(cert, key)", "TLS 加密；cert/key 均为字符串(PEM 文件路径)"]
  ]},
  quantize: { desc: "模型量化", fns: [
    ["fp16_to_awq(weightsJson, bits, groupSize)", "FP16 转 AWQ；weightsJson 字符串(权重 JSON)，bits 整数(量化位数，如 4)，groupSize 整数(分组大小，如 128)"],["arch_template(archName)", "架构量化模板；archName 字符串"],["verify_precision(fp16Values, awqValues)", "精度对比；两个参数均为浮点列表"],["model_info(modelPath)", "量化模型信息；modelPath 字符串"]
  ]},
  engine: { desc: "推理引擎", fns: [
    ["init(configJson)", "初始化引擎；configJson 字符串(JSON 配置，可传 {} 使用默认值)"],["add_request(prompt, maxTokens, temperature, topP, topK)", "添加请求；格式同 inference.generate（prompt 字符串，maxTokens/topK 整数，temperature/topP 浮点）"],["step()", "执行一次推理步进"],["status()", "引擎状态"],["reset()", "重置引擎"],["serve(port)", "启动推理服务；port 整数(端口号)"]
  ]},
  sandbox: { desc: "沙箱执行环境", fns: [
    ["create()", "创建沙箱，返回实例句柄"],["set_cpu_limit(inst, ms)", "CPU 时间限制；inst 为 create 返回的实例，ms 整数(毫秒)"],["set_mem_limit(inst, bytes)", "内存限制；bytes 整数(字节)"],["set_max_pids(inst, n)", "最大进程数；n 整数"],["set_work_dir(inst, path)", "工作目录；path 字符串"],["set_binary(inst, path)", "执行程序路径；path 字符串"],["add_arg(inst, arg)", "添加启动参数；arg 字符串"],["set_stdin(inst, path)", "重定向标准输入；path 字符串(文件路径)"],["set_stdout(inst, path)", "重定向标准输出；path 字符串"],["set_stderr(inst, path)", "重定向标准错误；path 字符串"],["exec(inst)", "在沙箱中执行"],["destroy(inst)", "销毁沙箱实例"]
  ]},
  raft: { desc: "Raft 分布式共识", fns: [
    ["build(config_json)", "构建节点；config_json 字符串(JSON，形如 {\"node_id\":\"n1\",\"bind_addr\":\"127.0.0.1:9000\"})"],["erase(node_id)", "删除节点；node_id 字符串"],["status(node_id)", "节点状态；node_id 字符串"],["list_nodes()", "列出全部节点"],["add(node_id, cmd_type, payload)", "追加日志；三个参数均为字符串"],["register_handler(node_id, ...)", "注册消息处理器"],["register_node(node_id, role, addr)", "注册集群节点；role 字符串(\"leader\"/\"follower\")，addr 字符串(\"IP:端口\")"],["cluster_nodes(node_id)", "集群节点列表"],["mount_storage(node_id, name, path)", "挂载存储；三个参数均为字符串"],["plugin_load(node_id, name, so_path)", "加载插件；so_path 字符串(.so 文件路径)"],["plugin_unload(node_id, name)", "卸载插件"],["plugin_list(node_id)", "已加载插件列表"],["leader_info(node_id)", "主节点信息"],["send(node_id, msg)", "发送消息；msg 字符串"],["recv(node_id)", "接收消息"]
  ]},
  cluster: { desc: "集群管理", fns: [
    ["connect(name, addr, secret)", "加入集群；三个参数均为字符串，addr 形如 \"IP:端口\""],["disconnect(name)", "断开集群；name 字符串"],["list()", "列出全部集群"],["is_connected(name)", "是否已连接；返回布尔"],["status(name)", "集群状态"],["nodes(name)", "集群节点列表"],["health(name)", "集群健康检查；返回布尔"],["exec(name, cmd_type, payload)", "集群执行命令；三个参数均为字符串"],["query(name, path)", "集群查询；path 字符串(查询路径)"],["broadcast(cmd_type, payload)", "集群广播；两个参数均为字符串"],["health_all()", "全部集群健康检查"]
  ]},
  kv: { desc: "KV 节点与存储", fns: [
    ["build(node_id, binary, args_json, work_dir, addr)", "构建 KV 节点；五个参数均为字符串，args_json 为 JSON 数组文本（如空数组），addr 形如 \"IP:端口\""],["add(node_id, binary, args_json, work_dir, addr)", "添加节点；格式同 build"],["erase(node_id)", "删除节点"],["skip(node_id)", "跳过节点"],["sleep(node_id)", "休眠节点"],["wakeup(node_id)", "唤醒节点"],["stop(node_id)", "停止节点"],["restart(node_id)", "重启节点"],["status(node_id)", "节点状态"],["list()", "节点列表"],["alive(node_id)", "节点存活检查；返回布尔"],["bind_sandbox(node_id, ...)", "绑定沙箱"],["get_sandbox(node_id)", "获取绑定的沙箱"],["unbind_sandbox(node_id)", "解绑沙箱"],["add_box(node_id, box, addr)", "添加盒子；box 为值对象(任意类型)，addr 字符串"],["locate(box)", "定位盒子所在节点；box 为值对象"],["self()", "当前节点标识(读取环境变量 XMEX_NODE_ID)"],["iter()", "创建迭代器，返回迭代器对象"],["next(it)", "迭代器前移；it 为 iter() 返回的对象"],["rewind(it)", "重置迭代器"],["iter_destroy(it)", "销毁迭代器"],["iter_live(it)", "迭代器是否有效；返回布尔"]
  ]}
};
