# Releases et versioning

## Strategie de version

`nidmi-core` suit SemVer:

- `MAJOR`: changement cassant de l'API publique
- `MINOR`: ajout retrocompatible
- `PATCH`: correctif interne sans impact API

Exemples:

- `0.1.0` premier contrat stable
- `0.2.0` nouvelle capacite non cassante
- `1.0.0` API consideree mature

## Regles de compatibilite

- `NiDMI` et `NiDMI-Player` doivent pinner une version explicite de `nidmi-core`.
- Eviter les dependances flottantes sur `main`.
- Toute modification d'API doit etre documentee dans un changelog.

## Politique de branches (recommandee)

- `main`: etat stable
- `feat/*`: nouvelles fonctionnalites
- `fix/*`: correctifs
- `release/*`: preparation release

## Changelog

Format recommande:

```markdown
## [0.2.0] - YYYY-MM-DD
### Added
- ...
### Changed
- ...
### Fixed
- ...
### Breaking
- ...
```

## Process de release

1. verifier tests/build des projets dependants (`NiDMI`, `NiDMI-Player`)
2. mettre a jour `README` et `docs/API.md`
3. mettre a jour `CHANGELOG.md`
4. creer tag Git (`vX.Y.Z`)
5. publier notes de release

## Politique de deprecation

- une API depreciee reste disponible au moins une version mineure
- la doc indique:
  - date d'introduction de la deprecation
  - alternative recommandee
  - version cible de suppression
