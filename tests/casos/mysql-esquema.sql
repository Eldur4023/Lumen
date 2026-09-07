-- Esquema de pruebas del modulo mysql.
--
-- Cubre a proposito los tipos que mas facilmente se traducen mal, y una columna
-- de texto LARGA: el driver pide los resultados como texto con un buffer fijo,
-- asi que si el buffer se queda corto tiene que notarse aqui y no en produccion.

DROP TABLE IF EXISTS tipos;
DROP TABLE IF EXISTS cuentas;
DROP TABLE IF EXISTS articulos;

CREATE TABLE articulos (
    id       BIGINT AUTO_INCREMENT PRIMARY KEY,
    titulo   VARCHAR(200)  NOT NULL,
    cuerpo   TEXT,                       -- aqui van los 4 KB
    autor    VARCHAR(100),
    vistas   INT           NOT NULL DEFAULT 0,
    creado   DATETIME      NOT NULL DEFAULT CURRENT_TIMESTAMP
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE cuentas (
    id     BIGINT AUTO_INCREMENT PRIMARY KEY,
    nombre VARCHAR(100) NOT NULL,
    saldo  INT          NOT NULL
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Una fila por cada tipo que puede salir mal al convertir.
CREATE TABLE tipos (
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
    t_nulo    VARCHAR(50)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

INSERT INTO tipos
  (t_tiny, t_bool, t_small, t_int, t_big, t_ubig, t_float, t_double, t_decimal,
   t_char, t_varchar, t_text, t_date, t_time, t_dt, t_ts, t_year, t_blob, t_json, t_nulo)
VALUES
  (-128, TRUE, -32768, -2147483648, -9223372036854775808, 18446744073709551615,
   1.5, 3.141592653589793, 12345678.9012,
   'fijo', 'con acentos: añoñó y un emoji 🐻', 'texto corto',
   '2026-08-31', '13:45:59', '2026-08-31 13:45:59', '2026-08-31 13:45:59',
   2026, 'bytes', '{"a": 1, "b": [2, 3]}', NULL);

-- Fila para el byte nulo dentro de un texto: LENGTH() de MySQL cuenta bytes de
-- verdad, sin pararse en el primer NUL como sqlite. Lo que esto comprueba es
-- que el DRIVER no trunca al convertir el resultado a std::string -- ya se
-- arreglo una vez para el texto largo (STMT_ATTR_UPDATE_MAX_LENGTH), y esto es
-- la version corta del mismo riesgo: un limite de tamano mal calculado.
INSERT INTO tipos (id, t_text) VALUES (2, CAST(x'6100620063' AS CHAR));

-- Fila con el texto largo: 4000 caracteres, muy por encima del buffer de 1024.
INSERT INTO articulos (titulo, cuerpo, autor, vistas)
VALUES ('largo', REPEAT('0123456789', 400), 'Ana', 1);

INSERT INTO articulos (titulo, cuerpo, autor, vistas)
VALUES ('corto', 'un cuerpo breve', 'Bob', 2);

INSERT INTO articulos (titulo, cuerpo, autor, vistas)
VALUES ('unicode ñ 🐻', 'cuerpo con ñ y 🐻', 'Cé', 3);

INSERT INTO cuentas (nombre, saldo) VALUES ('ana', 100), ('bob', 100);
