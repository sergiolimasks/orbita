"""Acesso ao Postgres (psycopg 3 + pool)."""
import os
from psycopg.rows import dict_row
from psycopg_pool import ConnectionPool

_pool = None


def init(dsn, max_size=8):
    global _pool
    if _pool is not None:
        _pool.close()
    _pool = ConnectionPool(dsn, min_size=1, max_size=max_size, kwargs={"row_factory": dict_row},
                           open=True)


def conn():
    """Conexão do pool; o bloco `with` faz commit no fim (rollback se der erro)."""
    return _pool.connection()


def rows(sql, *a):
    with _pool.connection() as c:
        return c.execute(sql, a).fetchall()


def row(sql, *a):
    with _pool.connection() as c:
        return c.execute(sql, a).fetchone()


def run(sql, *a):
    with _pool.connection() as c:
        return c.execute(sql, a).rowcount


def apply_schema():
    path = os.path.join(os.path.dirname(__file__), "schema.sql")
    sql = open(path, encoding="utf-8").read()
    with _pool.connection() as c:
        c.execute("SELECT pg_advisory_xact_lock(36036)")
        c.execute(sql)
