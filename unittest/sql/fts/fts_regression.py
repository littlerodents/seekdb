#!/usr/bin/env python3
"""Exercise FTS semantics through a running seekdb, including concurrent tokenization.

This creates a uniquely named database containing only synthetic data and leaves it
for inspection. Set MYSQL_ARGS to override the default local host/port/user.
"""
from __future__ import annotations

from concurrent.futures import ThreadPoolExecutor
import os
import shlex
import subprocess
import time

MYSQL = ["mysql", *shlex.split(os.environ.get("MYSQL_ARGS", "-h127.0.0.1 -P2881 -uroot")),
         "-N", "-s", "--default-character-set=utf8mb4"]
CASES = (
    "SELECT tokenize('全文索引的分词器热路径优化，数据库性能与数据性能。', 'ik');",
    "SELECT tokenize('中华人民共和国人民大会堂，混合 OceanBase 与 English。', 'ik');",
    "SELECT tokenize('The quick brown fox and the indexing engine.', 'beng');",
    "SELECT tokenize('alpha beta alpha delta', 'space');",
    "SELECT tokenize('中文分词性能测试及Unicode😀', 'ngram');",
    "SELECT tokenize('中文分词性能测试及Unicode😀', 'ngram2');",
)


def run(sql: str) -> str:
    result = subprocess.run(
        MYSQL, input="SET NAMES utf8mb4;\n" + sql, text=True, encoding="utf-8",
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=True,
    )
    return result.stdout


def main() -> None:
    expected = run("\n".join(CASES))
    repetitions = 20
    with ThreadPoolExecutor(max_workers=8) as executor:
        futures = [executor.submit(run, "\n".join(CASES * repetitions)) for _ in range(8)]
        for future in futures:
            assert future.result() == expected * repetitions, "concurrent TOKENIZE changed output"
    print(f"PASS concurrent TOKENIZE: {len(CASES) * repetitions * 8} calls")

    database = f"fts_regression_{time.time_ns()}"
    output = run(f"""
        CREATE DATABASE {database};
        USE {database};
        CREATE TABLE docs(id BIGINT PRIMARY KEY, content TEXT) DEFAULT CHARSET=utf8mb4;
        INSERT INTO docs VALUES (1,'全文索引加速检索'), (2,'独立数据资料');
        ALTER TABLE docs ADD FULLTEXT INDEX idx_content(content) WITH PARSER ik;
        SELECT 'built', COALESCE(GROUP_CONCAT(id ORDER BY id), 'empty') FROM docs
          WHERE MATCH(content) AGAINST('全文索引');
        INSERT INTO docs VALUES (3,'新增全文索引说明');
        SELECT 'inserted', COALESCE(GROUP_CONCAT(id ORDER BY id), 'empty') FROM docs
          WHERE MATCH(content) AGAINST('全文索引');
        UPDATE docs SET content='修改后的非目标文本' WHERE id=1;
        SELECT 'updated', COALESCE(GROUP_CONCAT(id ORDER BY id), 'empty') FROM docs
          WHERE MATCH(content) AGAINST('全文索引');
        DELETE FROM docs WHERE id=3;
        SELECT 'deleted', COALESCE(GROUP_CONCAT(id ORDER BY id), 'empty') FROM docs
          WHERE MATCH(content) AGAINST('全文索引');
    """)
    assert output.splitlines() == ["built\t1", "inserted\t1,3", "updated\t3", "deleted\tempty"], output
    print(f"PASS index build, insert, update, delete and MATCH; database retained: {database}")


if __name__ == "__main__":
    main()
