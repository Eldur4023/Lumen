-- Test schema for the mysql module.
--
-- It deliberately covers the types that are most easily mistranslated, and a
-- LONG text column: the driver asks for results as text with a fixed buffer,
-- so if the buffer falls short it has to show up here and not in production.

DROP TABLE IF EXISTS types;
DROP TABLE IF EXISTS accounts;
DROP TABLE IF EXISTS articles;

CREATE TABLE articles (
    id       BIGINT AUTO_INCREMENT PRIMARY KEY,
    title   VARCHAR(200)  NOT NULL,
    body   TEXT,                       -- the 4 KB go here
    author    VARCHAR(100),
    views   INT           NOT NULL DEFAULT 0,
    creado   DATETIME      NOT NULL DEFAULT CURRENT_TIMESTAMP
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE accounts (
    id     BIGINT AUTO_INCREMENT PRIMARY KEY,
    name VARCHAR(100) NOT NULL,
    balance  INT          NOT NULL
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- One row per type that can go wrong when converting.
CREATE TABLE types (
    id        INT AUTO_INCREMENT PRIMARY KEY,
    t_tiny    TINYINT,
    t_bool    BOOLEAN,
    t_small   SMALLINT,
    t_int     INT,
    t_big     BIGINT,
    t_ubig    BIGINT UNSIGNED,
    t_float   FLOAT,
    t_double  DOUBLE,
    t_decimal DECIMAL(18,4),
    t_char    CHAR(10),
    t_varchar VARCHAR(200),
    t_text    TEXT,
    t_date    DATE,
    t_time    TIME,
    t_dt      DATETIME,
    t_ts      TIMESTAMP NULL,
    t_year    YEAR,
    t_blob    BLOB,
    t_json    JSON,
    t_null    VARCHAR(50)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

INSERT INTO types
  (t_tiny, t_bool, t_small, t_int, t_big, t_ubig, t_float, t_double, t_decimal,
   t_char, t_varchar, t_text, t_date, t_time, t_dt, t_ts, t_year, t_blob, t_json, t_null)
VALUES
  (-128, TRUE, -32768, -2147483648, -9223372036854775808, 18446744073709551615,
   1.5, 3.141592653589793, 12345678.9012,
   'fixed', 'with accents: añoñó and an emoji 🐻', 'short text',
   '2026-08-31', '13:45:59', '2026-08-31 13:45:59', '2026-08-31 13:45:59',
   2026, 'bytes', '{"a": 1, "b": [2, 3]}', NULL);

-- Row for the null byte inside a text: MySQL's LENGTH() counts real bytes,
-- without stopping at the first NUL like sqlite.  What this checks is that the
-- DRIVER does not truncate when converting the result to std::string -- it was
-- already fixed once for the long text (STMT_ATTR_UPDATE_MAX_LENGTH), and this is
-- the short version of the same risk: a badly computed size limit.
INSERT INTO types (id, t_text) VALUES (2, CAST(x'6100620063' AS CHAR));

-- Row with the long text: 4000 characters, well above the 1024 buffer.
INSERT INTO articles (title, body, author, views)
VALUES ('length', REPEAT('0123456789', 400), 'Ana', 1);

INSERT INTO articles (title, body, author, views)
VALUES ('short', 'un body breve', 'Bob', 2);

INSERT INTO articles (title, body, author, views)
VALUES ('unicode ñ 🐻', 'body with ñ and 🐻', 'Cé', 3);

INSERT INTO accounts (name, balance) VALUES ('ana', 100), ('bob', 100);
