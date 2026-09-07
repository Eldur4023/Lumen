-- Test schema for the postgres module.
--
-- It deliberately covers what is most easily mistranslated: the integer
-- limits, numeric precision, the special floating point values that ONLY
-- postgres accepts —NaN and Infinity—, a long text, and the types the
-- driver lets through as text.

DROP TABLE IF EXISTS special;
DROP TABLE IF EXISTS types;
DROP TABLE IF EXISTS accounts;
DROP TABLE IF EXISTS articles;

CREATE TABLE articles (
    id      bigserial PRIMARY KEY,
    title  varchar(200) NOT NULL,
    body  text,                      -- the 4 KB go here
    author   varchar(100),
    views  integer NOT NULL DEFAULT 0,
    creado  timestamp NOT NULL DEFAULT now()
);

CREATE TABLE accounts (
    id     bigserial PRIMARY KEY,
    name varchar(100) NOT NULL,
    balance  integer NOT NULL
);

CREATE TABLE types (
    id         serial PRIMARY KEY,
    t_bool     boolean,
    t_small    smallint,
    t_int      integer,
    t_big      bigint,
    t_real     real,
    t_double   double precision,
    t_numeric  numeric(30,10),
    t_char     char(10),
    t_varchar  varchar(200),
    t_text     text,
    t_date     date,
    t_time     time,
    t_ts       timestamp,
    t_tstz     timestamptz,
    t_json     json,
    t_jsonb    jsonb,
    t_uuid     uuid,
    t_bytea    bytea,
    t_array    integer[],
    t_null     varchar(50)
);

INSERT INTO types
  (t_bool, t_small, t_int, t_big, t_real, t_double, t_numeric,
   t_char, t_varchar, t_text, t_date, t_time, t_ts, t_tstz,
   t_json, t_jsonb, t_uuid, t_bytea, t_array, t_null)
VALUES
  (true, -32768, -2147483648, -9223372036854775808,
   1.5, 3.141592653589793, 12345678901234.1234567890,
   'fixed', 'with accents: añoñó and an emoji 🐻', 'short text',
   '2026-08-31', '13:45:59', '2026-08-31 13:45:59', '2026-08-31 13:45:59+00',
   '{"a": 1}', '{"b": [2, 3]}', '00000000-0000-0000-0000-000000000001',
   '\x68656c6c6f', '{1,2,3}', NULL);

-- The special floating point values.  MySQL does not even accept them;
-- postgres does, and what comes out through the JSON has to be seen.
CREATE TABLE special (
    kind      text,
    d        double precision,
    n        numeric
);
INSERT INTO special VALUES
  ('nan',      'NaN'::float8,        'NaN'::numeric),
  ('inf',      'Infinity'::float8,   NULL),
  ('menosinf', '-Infinity'::float8,  NULL);

-- Texto length: 4000 caracteres.
INSERT INTO articles (title, body, author, views)
VALUES ('length', repeat('0123456789', 400), 'Ana', 1);

INSERT INTO articles (title, body, author, views)
VALUES ('short', 'un body breve', 'Bob', 2);

INSERT INTO articles (title, body, author, views)
VALUES ('unicode ñ 🐻', 'body with ñ and 🐻', 'Cé', 3);

INSERT INTO accounts (name, balance) VALUES ('ana', 100), ('bob', 100);
