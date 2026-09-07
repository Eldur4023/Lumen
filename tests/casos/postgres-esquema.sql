-- Esquema de pruebas del modulo postgres.
--
-- Cubre a proposito lo que mas facilmente se traduce mal: los limites de los
-- enteros, la precision de numeric, los valores especiales de coma flotante que
-- SOLO postgres admite —NaN e Infinity—, un texto largo, y los tipos que el
-- driver deja pasar como cadena.

DROP TABLE IF EXISTS especiales;
DROP TABLE IF EXISTS tipos;
DROP TABLE IF EXISTS cuentas;
DROP TABLE IF EXISTS articulos;

CREATE TABLE articulos (
    id      bigserial PRIMARY KEY,
    titulo  varchar(200) NOT NULL,
    cuerpo  text,                      -- aqui van los 4 KB
    autor   varchar(100),
    vistas  integer NOT NULL DEFAULT 0,
    creado  timestamp NOT NULL DEFAULT now()
);

CREATE TABLE cuentas (
    id     bigserial PRIMARY KEY,
    nombre varchar(100) NOT NULL,
    saldo  integer NOT NULL
);

CREATE TABLE tipos (
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
    t_nulo     varchar(50)
);

INSERT INTO tipos
  (t_bool, t_small, t_int, t_big, t_real, t_double, t_numeric,
   t_char, t_varchar, t_text, t_date, t_time, t_ts, t_tstz,
   t_json, t_jsonb, t_uuid, t_bytea, t_array, t_nulo)
VALUES
  (true, -32768, -2147483648, -9223372036854775808,
   1.5, 3.141592653589793, 12345678901234.1234567890,
   'fijo', 'con acentos: añoñó y un emoji 🐻', 'texto corto',
   '2026-08-31', '13:45:59', '2026-08-31 13:45:59', '2026-08-31 13:45:59+00',
   '{"a": 1}', '{"b": [2, 3]}', '00000000-0000-0000-0000-000000000001',
   '\x68656c6c6f', '{1,2,3}', NULL);

-- Los valores especiales de coma flotante.  MySQL no los admite siquiera;
-- postgres si, y hay que ver que sale por el JSON.
CREATE TABLE especiales (
    que      text,
    d        double precision,
    n        numeric
);
INSERT INTO especiales VALUES
  ('nan',      'NaN'::float8,        'NaN'::numeric),
  ('inf',      'Infinity'::float8,   NULL),
  ('menosinf', '-Infinity'::float8,  NULL);

-- Texto largo: 4000 caracteres.
INSERT INTO articulos (titulo, cuerpo, autor, vistas)
VALUES ('largo', repeat('0123456789', 400), 'Ana', 1);

INSERT INTO articulos (titulo, cuerpo, autor, vistas)
VALUES ('corto', 'un cuerpo breve', 'Bob', 2);

INSERT INTO articulos (titulo, cuerpo, autor, vistas)
VALUES ('unicode ñ 🐻', 'cuerpo con ñ y 🐻', 'Cé', 3);

INSERT INTO cuentas (nombre, saldo) VALUES ('ana', 100), ('bob', 100);
