# MiniVectorDB

答卷实现：OceanBase 2026 加赛题 600081 —— 一个单机、单进程、单客户端连接的轻量向量数据库。
C++17 实现，零第三方依赖，单二进制。

## 构建与启动

```bash
docker build -t minivectordb:latest .
docker run -i --rm \
  --network=none --cpus=2 --memory=2g \
  --read-only --cap-drop=ALL --security-opt=no-new-privileges \
  --mount type=bind,src=/some/data/dir,dst=/data \
  minivectordb:latest
```

`-i` 不配 `-t`：判题器通过进程的 stdin/stdout 交流，stdout 只允许出现协议报文，
所有诊断走 stderr。数据文件统一写入 `/data`。

### 关于容器 uid

判题器以 `--cap-drop=ALL --read-only` 启动。丢能力会同时去掉 `CAP_DAC_OVERRIDE`
（只能写自己 uid 拥有的目录）和 `CAP_SETUID`（进程内无法再切换用户，实测
`setpriv` 报 `setresuid failed: Operation not permitted`），所以 uid 只能在构建时固定：

| 容器内 uid | /data 属主与模式 | 能力 | 结果 |
|---|---|---|---|
| 0 | 1001，0700 | 全部丢弃 | `EACCES` |
| 0 | 1001，0777 | 全部丢弃 | 可写 |
| 0 | 1001，0700 | 保留 | 可写 |
| 1001 | 1001，0700 | 全部丢弃 | 可写 |

镜像默认 `USER 1001:1001`（GitHub `ubuntu-latest` 的 runner uid，也是公开 checker
产生临时目录的属主），并把镜像内 `/data` `chown` 给该 uid——命名卷会继承这个属主，
所以命名卷路径在任何宿主上都能写。若判题机 uid 不同，用
`--build-arg APP_UID=$(id -u) --build-arg APP_GID=$(id -g)` 重建即可。

## 协议

启动后先完成恢复，再输出一行 `{"type":"ready"}` 并 flush，然后逐行读 JSON 请求、
逐行回 JSON 响应。

```jsonc
{"id":1,"op":"execute","sql":"SELECT id FROM docs ORDER BY id;","search_mode":"exact"}
{"id":1,"ok":true,"columns":["id"],"rows":[[1],[2]]}
{"id":2,"ok":true,"affected_rows":1}
{"id":3,"ok":false,"error":{"code":"duplicate_key","message":"duplicate primary key"}}
{"id":4,"op":"shutdown"}          // 回滚未提交事务、落盘、返回 {"id":4,"ok":true} 后退出
```

## 从空目录开始的操作示例

```
CREATE TABLE docs (id BIGINT PRIMARY KEY, tag VARCHAR(16), emb VECTOR(4));
INSERT INTO docs VALUES (1,'a','[1,0,0,0]'),(2,'b','[0,1,0,0]'),(3,'a','[0.9,0.1,0,0]');
SELECT id, L2_DISTANCE(emb,'[1,0,0,0]') AS d FROM docs ORDER BY d LIMIT 2;
SELECT id FROM docs WHERE tag='a' AND L2_DISTANCE(emb,'[1,0,0,0]') < 0.5 ORDER BY id;
BEGIN; DELETE FROM docs WHERE id=3; ROLLBACK;
SELECT COUNT(*) FROM docs;      -- 仍为 3
```

重启（复用同一 `/data`）后内容不变；`--restart` 或强制终止后未提交事务不生效。

## SQL 支持范围

| 能力 | 支持情况 |
|---|---|
| `CREATE TABLE` | `BIGINT`、`VARCHAR(n)`、`VECTOR(n)`，一个主键（可复合），每表至多一个向量列 |
| `INSERT` | 多行 VALUES；语句级原子（任一行失败整条回滚） |
| `SELECT` | `*` 与投影、`AS` 别名、`WHERE`（`=` `<` `<=` `>` `>=` `AND`）、`ORDER BY`（含 `L2_DISTANCE`）、`LIMIT`、`COUNT(*)` |
| `UPDATE` / `DELETE` | 按主键或条件；主键不可修改 |
| 向量检索 | `L2_DISTANCE` 与精确 TopK；带过滤时**先过滤再取 K**；距离并列按主键升序 |
| 事务 | `BEGIN` / `COMMIT` / `ROLLBACK`；事务内读自己的写；最多一个显式事务 |
| 索引 | `CREATE VECTOR INDEX` 被接受并维护元数据；`exact` 模式始终返回精确结果 |

**错误处理**：不支持的语句返回 `unsupported_sql`；重复主键 `duplicate_key`；维度不符、
NaN/Infinity、超出 float32 表示范围均拒绝。SQL 错误不终止进程，失败语句不留部分修改。

## 设计与验证记录

### 记录布局

`/data` 下两个文件：

- `snapshot.dat` —— 全量快照。布局 `magic | table_count | crc32(body) | body`，
  body 为表定义 + 全部行。写入走 `tmp + fsync + rename`，任何时刻磁盘上都有一个完整快照。
- `redo.log` —— 追加日志。每条记录 `magic | payload_len | crc32 | payload`。
  恢复时顺序重放；尾部被截断或 CRC 不符即停止重放并截断（依托强杀只会撕掉最后一条）。

### 事务与提交

自动提交语句直接写 redo 并 fsync。显式事务内改动只进内存，**不写 redo**；
`COMMIT` 时把攒下的 redo 记录一次性写入并 fsync，然后才输出成功响应——
所以"已确认提交"严格对应"已落盘"。`ROLLBACK` 丢弃缓冲并从快照重放未提交前的状态。
`shutdown` 先回滚未提交事务再写快照，因此正常关闭也不会把未提交状态写进磁盘。

### 向量检索

距离用 double 累加、四路独立累加器展开，避免 float32 累加误差影响 1e-5 容差下的比较。
`LIMIT` + `ORDER BY` 时用固定大小最大堆维护前 K，不整体排序；无 `LIMIT` 时才全排序。
带过滤的 TopK 先求过滤集合再在其内取前 K，不做"先全表取 K 再过滤"。

### 已验证

全部在**判题同构容器**（`--network=none --cpus=2 --memory=2g --read-only --cap-drop=ALL
--security-opt=no-new-privileges`）内完成，用题包自带 `protocol_check` 驱动：

- 握手 `{"type":"ready"}`：PASS
- 20/20 公开用例：`sql_crud`、`sql_comparisons`、`sql_ordering`、`sql_empty`、
  `sql_primary_key`、`sql_unicode`、`sql_bigint`、`sql_unsupported`、`storage_restart`、
  `vector_euclidean`、`transaction_commit`、`transaction_rollback`、`storage_mutations`、
  `storage_multiple_tables`、`vector_ties_and_filter`、`vector_float32`、
  `vector_index_maintenance`、`transaction_failed_insert`、`transaction_autocommit_atomic`、
  `transaction_shutdown_rollback`
- 硬杀恢复：提交后 `docker kill`，复用同一 `/data` 重启，读回 `[[1,"a"],[2,"b"]]`
- 未提交隔离：`BEGIN` 后写入、未 `COMMIT` 即 `docker kill`，重启后读回 `[[1]]`
- 两种 `/data` 挂载方式（0700 bind、root 命名卷）都能 ready

### 已知限制

1. **没有真正的 ANN 索引。** `CREATE VECTOR INDEX` 只维护元数据，`ann` 模式走精确扫描。
   召回率因此恒为 1.0（超过 0.95 的要求），但性能榜排名不会靠前——这是明确的取舍：
   功能准入是整题的硬门槛，性能是排名制，先保证零失分再谈名次。
2. **全文载入内存。** 题面要求支持"数据量超过内存限额"的场景，本实现每次查询基于内存
   数据集，大表场景会受 2 GiB 限制。公开用例未覆盖到该规模。
3. 不支持 `NULL`、`JOIN`、聚合（`COUNT(*)` 除外）、子查询、`ALTER TABLE`、嵌套事务、
   主键更新——与题面"不要求"的范围一致。
4. `VARCHAR` 比较按字节序而非 Unicode 码点序；ASCII 与 UTF-8 常见场景等价，
   混合非 ASCII 的边界情况未逐例验证。
