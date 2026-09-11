# core/graph：图与执行层。
#
# 目录结构是"层 → 模块"两级：
#   core/graph/ir/        中间表示（IR）—— 插件 API 本身
#   core/graph/structure/ 图结构：增删连接、版本号、无环校验
#   core/graph/mutate/    命令总线：全部变异的唯一入口 + 撤销栈
#   core/graph/validate/  加载期校验：类型、量纲、单位
#   core/graph/eval/      求值调度 + 内容寻址缓存
#   core/graph/domain/    三域语义：field / particle / render
#   core/graph/kernels/   原生算子契约（实现在插件）
#   core/graph/field/     场语义的最小契约

add_subdirectory(ir)
add_subdirectory(structure)
