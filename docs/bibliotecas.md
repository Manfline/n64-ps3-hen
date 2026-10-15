# rsxkit y spukit: repositorios propios

Las dos bibliotecas MIT de Salsa64 viven también en sus propios repositorios:

| Biblioteca | Repositorio | En Salsa64 |
|---|---|---|
| rsxkit | https://github.com/ricowp81/rsxkit | `lib/rsxkit/` |
| spukit | https://github.com/ricowp81/spukit | `lib/spukit/` |

Salsa64 las lleva con `git subtree`: los archivos están en el árbol de
Salsa64 como siempre, así que compilar Salsa64 no pide submódulos ni pasos
extra. Los repositorios propios llevan solo la historia de cada carpeta,
sacada con `git subtree split`, y cada uno tiene su `LICENSE` (MIT), su
`README.md` y su `THIRD_PARTY.md`.

## Reglas

- Las de siempre (`docs/THIRD_PARTY.md`): nada de `src/` en
  `lib/`, nada de código GPL, y todo origen registrado, ahora también en el
  `THIRD_PARTY.md` de la biblioteca.
- Se trabaja en Salsa64 como hasta ahora: un cambio en `lib/rsxkit` o en
  `lib/spukit` es un commit normal de Salsa64.
- Conviene que un commit que toca `lib/` no toque otras carpetas, para que
  la historia de la biblioteca quede limpia (si las mezcla, el commit
  aparece en la biblioteca solo con su parte de `lib/`).

## Mandar los cambios a los repositorios propios

Una vez por release, o cuando haya algo que valga la pena publicar:

```sh
git remote add rsxkit https://github.com/ricowp81/rsxkit.git   # una vez
git remote add spukit https://github.com/ricowp81/spukit.git   # una vez
git subtree push --prefix=lib/rsxkit rsxkit main
git subtree push --prefix=lib/spukit spukit main
```

## Traer cambios de los repositorios propios

Si alguien manda un pull request a rsxkit o a spukit y se acepta allá:

```sh
git subtree pull --prefix=lib/rsxkit rsxkit main
git subtree pull --prefix=lib/spukit spukit main
```

Después del pull, compilar Salsa64 y correr `test-host` y el gfxtest en
RPCS3 como con cualquier cambio de `lib/`.
