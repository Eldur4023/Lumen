// Prueba del traductor de marcadores de postgres, sin servidor de por medio.
//
// Incluye el .cpp del driver para llegar a la funcion, que vive en un espacio
// de nombres anonimo: asi se prueba EL codigo, no una copia suya que podria
// divergir sin que nadie se entere.
#include "../src/lumen_script/db_postgres.cpp"

#include <cstdio>
#include <string>

static int fallos = 0;

static void bien(const char* nombre, const std::string& sql, size_t nargs,
                 const std::string& esperado) {
    std::string out, err;
    if (!lumen_script::traducir_marcadores(sql, nargs, out, err)) {
        ++fallos;
        std::printf("  FALLA %s\n    error inesperado: %s\n", nombre, err.c_str());
        return;
    }
    if (out != esperado) {
        ++fallos;
        std::printf("  FALLA %s\n    esperado: <<%s>>\n    obtenido: <<%s>>\n",
                    nombre, esperado.c_str(), out.c_str());
        return;
    }
    std::printf("  ok    %s\n", nombre);
}

static void mal(const char* nombre, const std::string& sql, size_t nargs,
                const std::string& trozo) {
    std::string out, err;
    if (lumen_script::traducir_marcadores(sql, nargs, out, err)) {
        ++fallos;
        std::printf("  FALLA %s\n    paso cuando no debia: <<%s>>\n", nombre, out.c_str());
        return;
    }
    if (err.find(trozo) == std::string::npos) {
        ++fallos;
        std::printf("  FALLA %s\n    esperaba que dijera '%s'\n    dijo: %s\n",
                    nombre, trozo.c_str(), err.c_str());
        return;
    }
    std::printf("  ok    %s\n", nombre);
}

int main() {
    std::printf("== traduccion basica ==\n");
    bien("un marcador", "select * from t where id = ?", 1,
         "select * from t where id = $1");
    bien("tres marcadores", "select ? , ? where a = ?", 3,
         "select $1 , $2 where a = $3");
    bien("sin marcadores", "select 1", 0, "select 1");

    std::printf("== compatibilidad con el estilo de postgres ==\n");
    bien("ya venia con $1", "select * from t where id = $1", 1,
         "select * from t where id = $1");
    bien("$1 y $2", "insert into t values ($1, $2)", 2,
         "insert into t values ($1, $2)");

    std::printf("== lo que NO es un marcador ==\n");
    bien("dentro de una cadena", "select * from t where s = 'que?' and id = ?", 1,
         "select * from t where s = 'que?' and id = $1");
    bien("comilla escapada dentro", "select * from t where s = 'a''b?' and id = ?", 1,
         "select * from t where s = 'a''b?' and id = $1");
    bien("cadena con E y barra", "select * from t where s = E'a\\'?' and id = ?", 1,
         "select * from t where s = E'a\\'?' and id = $1");
    bien("identificador entrecomillado", "select \"col?\" from t where id = ?", 1,
         "select \"col?\" from t where id = $1");
    bien("comentario de linea", "-- que?\nselect ?", 1, "-- que?\nselect $1");
    bien("comentario de bloque", "/* ? */ select ?", 1, "/* ? */ select $1");
    bien("comentario anidado", "/* a /* ? */ ? */ select ?", 1,
         "/* a /* ? */ ? */ select $1");
    bien("bloque con $$", "select $$ ? $$ , ?", 1, "select $$ ? $$ , $1");
    bien("bloque con etiqueta", "select $x$ ? $x$ , ?", 1, "select $x$ ? $x$ , $1");

    std::printf("== el operador de JSONB ==\n");
    // Sin argumentos no se traduce nada: el `?` es el operador de JSONB.
    bien("sin argumentos, se deja igual", "select * from t where datos ? 'clave'", 0,
         "select * from t where datos ? 'clave'");
    // Con argumentos, `??` es la via de escape para ese operador.
    bien("?? es un ? literal", "select * from t where datos ?? 'k' and id = ?", 1,
         "select * from t where datos ? 'k' and id = $1");

    std::printf("== errores ==\n");
    mal("mezclar los dos estilos", "select * from t where a = $1 and b = ?", 2, "mezcla");
    mal("sobran argumentos", "select * from t where id = ?", 2, "1 marcador");
    mal("faltan argumentos", "select * from t where a = ? and b = ?", 1, "2 marcador");

    std::printf("\n%s\n", fallos ? "HAY FALLOS" : "todas pasan");
    return fallos ? 1 : 0;
}
