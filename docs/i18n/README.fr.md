![AllocSight Banner](assets/banner.png)

[![License: MIT](https://img.shields.io/badge/License-MIT-0284c7.svg?style=flat-square)](../../LICENSE)
[![Standard: C++17](https://img.shields.io/badge/C%2B%2B-17-4f46e5.svg?style=flat-square)](../../src/allocsight.cpp)
[![Platform: Windows x64](https://img.shields.io/badge/Platform-Windows%20x64-0f766e.svg?style=flat-square)](../../CMakeLists.txt)
[![Dependencies: Zero](https://img.shields.io/badge/Dependencies-None-059669.svg?style=flat-square)](../../build.ps1)

[**English**](../../README.md) | [**简体中文**](README.zh-CN.md) | [**Español**](README.es.md) | [**Français**](README.fr.md) | [**Русский**](README.ru.md) | [**العربية**](README.ar.md)

---

## Table des Matières

- [Présentation Générale](#présentation-générale)
- [Capacités Techniques Clés](#capacités-techniques-clés)
- [Architecture du Système](#architecture-du-système)
- [Benchmarks de Performance](#benchmarks-de-performance)
- [Interface en Ligne de Commande](#interface-en-ligne-de-commande)
- [Grammaire des Filtres de Requête](#grammaire-des-filtres-de-requête)
- [Heuristiques de Diagnostic d'Espace](#heuristiques-de-diagnostic-despace)
- [Intégration Agents IA et JSON](#intégration-agents-ia-et-json)
- [Compilation depuis les Sources](#compilation-depuis-les-sources)
- [Licence](#licence)

---

## Présentation Générale

**AllocSight** est un analyseur d'allocation de clusters de disque à haute concurrence et un moteur automatisé de récupération d'espace en ligne de commande pour Windows, sans aucune dépendance externe. Écrit en C++17 natif directement au-dessus des API de gestion de fichiers Win32, il est conçu pour les ingénieurs logiciels, les administrateurs système et les agents IA autonomes nécessitant une télémétrie de stockage en une fraction de seconde, sans la surcharge liée au rendu graphique.

Contrairement aux utilitaires classiques qui ne rapportent que la taille logique des fichiers (`nFileSizeLow` / `nFileSizeHigh`), AllocSight calcule **l'allocation physique réelle des clusters NTFS**, en prenant en compte l'alignement des clusters du système de fichiers, la compression NTFS transparente (LZNT1/XPRESS), les fichiers clairsemés (sparse files), les fichiers de rappel cloud et les flux de données alternatifs (ADS).

![AllocSight Terminal Output](assets/terminal-demo.png)

---

## Capacités Techniques Clés

1. **Énumération de Répertoires Kernel Multi-Threads**
   Déploie un pool de 8 à 16 threads de travail sur une file d'attente à contention minimale (`ParallelScanner`). Chaque thread appelle `FindFirstFileExW` avec `FindExInfoBasic` (en ignorant la recherche des noms courts 8.3) et `FIND_FIRST_EX_LARGE_FETCH` (activant les lectures de tampons de répertoires larges dans le noyau).
2. **Comptabilité Physique Exacte des Clusters NTFS**
   - Aligne les allocations sur la taille exacte des clusters du volume cible (`GetDiskFreeSpaceW`).
   - Interroge la taille physique réelle sur disque via `GetCompressedFileSizeW` lorsque les attributs `FILE_ATTRIBUTE_COMPRESSED` ou `FILE_ATTRIBUTE_SPARSE_FILE` sont présents.
   - Identifie les fichiers résidant uniquement dans le cloud (`FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS`, `FILE_ATTRIBUTE_RECALL_ON_OPEN`, `FILE_ATTRIBUTE_OFFLINE`) comme occupant `0` octet physique local.
   - Énumère en option les flux de données alternatifs NTFS (`:$DATA`) via les API publiques `FindFirstStreamW` / `FindNextStreamW` (`--ads`).
3. **Prévention des Boucles sur les Points d'Analyse (Reparse Points)**
   Inspecte `WIN32_FIND_DATAW::dwReserved0` à l'aide des macros standard de `<winnt.h>` (`IsReparseTagNameSurrogate`, `IO_REPARSE_TAG_SYMLINK`, `IO_REPARSE_TAG_MOUNT_POINT`) afin d'empêcher toute récursion infinie causée par les liens symboliques ou les points de montage.
4. **Moteur Heuristique de Récupération d'Espace**
   Classe l'espace récupérable en deux niveaux d'action :
   - **`[SAFE]`** : Caches déterministes de gestionnaires de paquets (`uv`, `pip`, `conda/pkgs`, `npm/_cacache`, `pnpm-cache`), caches de shaders (`DXCache`, `ShaderCache`), vidages sur incident (crash dumps) et répertoires temporaires orphelins.
   - **`[REVIEW]`** : Archives déjà extraites dans un dossier frère portant le même nom de base, dossiers de copie dupliqués, artefacts de compilation reconstructibles (`target`, `node_modules`, `.venv`) et fichiers volumineux non modifiés depuis plus de 180 jours.
5. **Rapports Markdown et JSON à Navigation Directe**
   Génère des tableaux Markdown structurés (`allocsight report`) et des flux JSON (`-j`) dans lesquels chaque chemin est accompagné d'un URI `file:///` encodé selon la norme RFC 8089 permettant une ouverture en un clic.

---

## Architecture du Système

![AllocSight System Architecture](assets/architecture.png)

Le code source est organisé sous forme de pipeline modulaire d'en-têtes C++17 dans [`src/`](../../src/) :

| Module | Responsabilité Principale |
| :--- | :--- |
| [`src/alloc_types.hpp`](../../src/alloc_types.hpp) | Arbre `FsNode`, géométrie d'alignement des clusters `VolumeMetrics`, acquisition du privilège `SeBackupPrivilege`, conversion UTF-8/UTF-16 et encodage URI `file:///`. |
| [`src/alloc_scanner.hpp`](../../src/alloc_scanner.hpp) | File de travail multi-threads `ParallelScanner`, énumération `FindFirstFileExW` à grand tampon, protection contre les cycles de liens symboliques et `NtfsStreamInspector`. |
| [`src/alloc_filter.hpp`](../../src/alloc_filter.hpp) | Moteur `QueryFilter` avec correspondance de motifs itérative à deux pointeurs (`wildcardMatch`), vecteurs de prédicats `MetricPredicate` / `AttributePredicate` et élagage d'arbres. |
| [`src/alloc_views.hpp`](../../src/alloc_views.hpp) | Moteurs de rendu `ReportEngine` (`drives`, `tree`, `top`, `categories`, `analyze`, `report`) et règles heuristiques de nettoyage. |
| [`src/allocsight.cpp`](../../src/allocsight.cpp) | Point d'entrée CLI Unicode (`wmain`), analyse des options et télémétrie d'exécution. |

---

## Benchmarks de Performance : AllocSight face aux Méthodes d'Analyse IA

Évalué sur un jeu de données standard de **1 000 000 de fichiers (500 Go NTFS)** sous Windows x64, AllocSight surpasse les méthodes d'analyse habituellement employées par les agents IA (PowerShell, Python, Node.js et clones `du`) d'un facteur **22x à 66x**, tout en éliminant les faux positifs liés aux fichiers cloud et à la compression NTFS :

![AllocSight Benchmark Comparison](assets/benchmark.png)

| Approche / Outil d'Analyse | Latence (1M Fichiers) | Modèle de Concurrence & E/S | Précision Physique des Clusters NTFS | Protection Boucles Symlink | Intelligence de Nettoyage IA & Sortie |
| :--- | :---: | :--- | :--- | :---: | :--- |
| **AllocSight (C++17 Natif)** | **2.8 s** *(1x Réf.)* | **File à 16 Threads** + `FindFirstFileExW` (`LARGE_FETCH`) | **Oui** (`GetCompressedFileSizeW` + Cloud Recall `0 o` + ADS) | **Filtrage Noyau Strict** (`IsReparseTagNameSurrogate`) | **Heuristiques `[SAFE]`/`[REVIEW]` + Markdown `file:///` & JSON** |
| **Clones CLI `du` (`dust`)** | **9.8 s** *(3.5x plus lent)* | Parcours multi-threads | **Partielle** (Surcompte les fichiers OneDrive non téléchargés ; sans ADS) | Oui | **Aucune** (Tailles brutes uniquement ; sans règles ni liens `file:///`) |
| **Node.js (`fs.promises` / `fast-glob`)** | **44.0 s** *(15.7x plus lent)* | Pool `libuv` + pression mémoire du tas V8 | **Non** (Taille logique uniquement ; ignore l'alignement des clusters) | Partielle | **Aucune** (Nécessite un script ad-hoc à chaque session) |
| **Python (`os.walk` / `pathlib.rglob`)** | **62.5 s** *(22.3x plus lent)* | Monothread limité par le GIL (`os.scandir` + `stat()`) | **Non** (Taille logique `st_size` ; fausse les fichiers compressés et cloud) | Partielle (`followlinks=False`) | **Aucune** (Coût élevé en tokens LLM pour coder des scripts temporaires) |
| **PowerShell (`Get-ChildItem -Recurse`)** | **185.0 s+** *(66x plus lent / Timeout)* | Instanciation monothread d'objets `.NET FileInfo` | **Non** (`Length` logique uniquement ; forte surcharge mémoire et GC) | **Non sécurisé** (Suit les jonctions par défaut) | **Aucune** (Sature la fenêtre de contexte de l'agent IA) |

---

## Interface en Ligne de Commande

### Synopsis

```text
allocsight <commande> [chemin] [options]
```

### Commandes

| Commande | Description |
| :--- | :--- |
| `drives` | Énumère tous les volumes logiques montés avec le type de système de fichiers, la taille de cluster, la capacité et le taux d'occupation. |
| `tree <chemin>` | Affiche un arbre hiérarchique proportionnel trié par allocation physique de clusters. |
| `top <chemin>` | Liste les Top-N fichiers et/ou répertoires les plus volumineux du sous-arbre analysé. |
| `categories <chemin>` | Agrège l'espace physique et logique par catégories de fichiers (IA/développement) et par extensions. |
| `analyze <chemin>` | Exécute le diagnostic heuristique de nettoyage (`[SAFE]` vs `[REVIEW]`) en console ou en JSON. |
| `report <chemin>` | Génère un rapport Markdown complet avec des liens `file:///` cliquables. |

### Options

| Option | Argument | Défaut | Description |
| :--- | :--- | :--- | :--- |
| `-f`, `--filter` | `<expr>` | *(aucun)* | Applique des prédicats de filtrage séparés par des points-virgules avant l'agrégation. |
| `-d`, `--depth` | `<int>` | `2` | Profondeur maximale de l'arbre de répertoires en mode `tree`. |
| `-n`, `--top` | `<int>` | `20` | Nombre maximal d'entrées affichées par niveau ou tableau. |
| `-m`, `--min-size` | `<size>` | `0` | Taille minimale allouée pour afficher une entrée (ex. `50mb`, `1gb`). |
| `-t`, `--threads` | `<int>` | `auto` | Nombre de threads de travail (par défaut : concurrence matérielle bornée à `8..16`). |
| `-o`, `--output` | `<file>` | `stdout` | Écrit la sortie directement dans le fichier spécifié (UTF-8). |
| `--files` | *(aucun)* | `false` | Restreint la sortie de `top` ou `tree` aux fichiers réguliers uniquement. |
| `--folders` | *(aucun)* | `false` | Restreint la sortie de `top` ou `tree` aux répertoires uniquement. |
| `--ads` | *(aucun)* | `false` | Inspecte les flux de données alternatifs NTFS via `FindFirstStreamW`. |
| `--logical` | *(aucun)* | `false` | Trie et affiche selon la taille logique au lieu de la taille allouée en clusters. |
| `-j`, `--json` | *(aucun)* | `false` | Émet une sortie JSON structurée pour les scripts ou les agents IA. |
| `-q`, `--quiet` | *(aucun)* | `false` | Supprime le résumé de télémétrie sur `stderr`. |

---

## Grammaire des Filtres de Requête

Les expressions de filtre (`-f "<expr>"`) sont composées d'une ou plusieurs clauses séparées par un point-virgule (`;`). Préfixez une clause avec `!` ou `-` pour inverser la condition (exclusion).

| Type de Clause | Syntaxe | Exemples | Sémantique |
| :--- | :--- | :--- | :--- |
| **Motif de Fichier** | `<motif>` | `*.gguf;*.safetensors` / `!*.log` | Correspondance de caractères génériques (`*`, `?`) par algorithme itératif à deux pointeurs. |
| **Motif de Répertoire** | `dir:<motif>` ou `<motif>/` | `dir:node_modules;dir:.venv` / `!dir:windows` | Inclut ou exclut les fichiers situés dans des répertoires parents correspondants. |
| **Borne de Taille** | `[métrique]<op><val><unité>` | `>100mb` / `allocated>1gb` / `logical<4kb` | Métriques : `allocated` (`alloc`, `cluster`, `disk`), `logical` (`log`, `size`). Unités : `b`, `kb`, `mb`, `gb`, `tb`. |
| **Borne d'Ancienneté** | `[champ]<op><val><unité>` | `>6months` / `created>30days` / `accessed<7d` | Champs : `modified` (`mtime`, `mod`, `age`), `created` (`ctime`), `accessed` (`atime`). Unités : `s`, `m`, `h`, `d`, `w`, `mo`, `y`. |
| **Catégorie** | `category:<nom>` | `category:AI Models` / `!category:Archives` | Filtre selon les groupes de fichiers intégrés (modèles IA, archives, disques virtuels, etc.). |
| **Attributs** | `attr:<drapeaux>` | `attr:hidden+system` / `attr:sparse-readonly` | Drapeaux : `archive`, `system`, `readonly`, `hidden`, `compressed`, `encrypted`, `offline`, `temporary`, `sparse`, `ads`. |

---

## Heuristiques de Diagnostic d'Espace

| Identifiant de Règle | Niveau | Logique de Détection |
| :--- | :---: | :--- |
| `recycle_bin` | `SAFE` | Conteneurs système `$Recycle.Bin` ou `RECYCLER` non vides. |
| `safe_temp_cache` | `SAFE` | Répertoires de cache/temporaires connus (`temp`, `tmp`, `cache`, `dxcache`, `_cacache`, `pnpm-cache`, `squirreltemp`, `crashpad`, `shadercache`, `gpucache`) $\ge 10\text{ Mo}$. |
| `orphan_staging` | `SAFE` | Répertoires temporaires d'installateurs interrompus ou profils de navigateur d'automatisation $\ge 20\text{ Mo}$. |
| `logs_and_temp_files` | `SAFE` | Fichiers individuels `.log`, `.dmp`, `.tmp`, `.bak`, `.old`, `.crdownload`, `.ushaderprecache` $\ge 10\text{ Mo}$. |
| `already_extracted_archive` | `REVIEW` | Archive (`.zip`, `.7z`, `.rar`, `.tar.gz`, `.tgz`) $\ge 50\text{ Mo}$ dont le nom de base correspond à un répertoire frère déjà extrait. |
| `duplicate_copy_folder` | `REVIEW` | Répertoires comportant un suffixe de copie (`- Copy`, `- 副本`, `old_backup`) $\ge 50\text{ Mo}$. |
| `dev_artifacts` | `REVIEW` | Arborescences de dépendances ou de build reconstructibles (`node_modules`, `.venv`, `venv`, `__pycache__`, `target`) $\ge 50\text{ Mo}$. |
| `large_archives_installers` | `REVIEW` | Images disque, paquets wheel, symboles de débogage ou installateurs (`.iso`, `.whl`, `.conda`, `.msi`, `.pdb`) $\ge 100\text{ Mo}$. |
| `stale_large_files` | `REVIEW` | Fichiers individuels $\ge 250\text{ Mo}$ non modifiés depuis plus de 180 jours. |

---

## Intégration Agents IA et JSON

L'ajout de `-j` / `--json` à n'importe quelle commande produit un flux JSON UTF-8 strictement échappé sur `stdout` incluant les champs `path` et `fileUri`. Consultez [`SKILL.md`](../../SKILL.md) pour plus de détails.

---

## Compilation depuis les Sources

### Option 1 : Script PowerShell (MinGW-w64)

```powershell
.\build.ps1
```

Ou directement avec `g++` :

```powershell
g++ -O3 -s -std=c++17 -municode -static src/allocsight.cpp -o allocsight.exe
```

### Option 2 : CMake (MSVC ou MinGW-w64)

```powershell
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

---

## Licence

Distribué sous les termes de la [Licence MIT](../../LICENSE). Copyright (c) 2026 AllocSight Contributors.
