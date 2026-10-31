[English](README.md) | 한국어

# gong2 Playlist Source for OBS

<p align="center">
  <img src="assets/logo.png" alt="gong2 Playlist Source 로고" width="160" height="160">
</p>

VLC 비디오 소스를 대체하는 OBS 재생목록 소스입니다. 내부적으로 OBS 기본 **미디어 소스(ffmpeg_source)** 를 사용하므로 VLC 설치가 필요 없고, 하드웨어 디코딩·HDR·속도 조절 등 미디어 소스의 기능을 그대로 씁니다.

## 기능

- **파일 / 폴더 / URL 추가** — 폴더를 추가하면 폴더 안의 미디어 파일이 파일 이름 순(자연 정렬: `ep2` < `ep10`)으로 재생됩니다. 폴더 내용은 폴더를 추가할 때(또는 OBS를 시작할 때) 읽고, 이후에는 폴더를 오른쪽 클릭해 *이 폴더 다시 읽기*를 선택할 때만 다시 읽습니다.
- **재생 중 목록 편집** — 항목 추가·삭제·순서 변경을 해도 현재 재생 중인 영상은 끊기지 않습니다. 재생 중인 파일이 목록에서 사라지면 같은 위치의 다음 항목을 재생합니다.
- **원하는 영상 선택 재생** — 재생목록 창에서 더블 클릭.
- **OBS 재시작 시 재생 위치 복원** — 종료 시점의 파일과 재생 위치(일시정지 상태 포함)를 기억했다가 이어서 재생합니다. 장면 컬렉션 저장과 별도로 5초마다 `resume.json`에도 기록하므로 OBS가 비정상 종료돼도 위치가 남습니다.
- **재생목록 창** — 도구 → *gong2 재생목록 창*, 또는 소스 속성의 *재생목록 창 열기* 버튼. OBS 독(dock)이라 별도 창으로 띄우거나 메인 창에 붙일 수 있습니다.
  - 현재 재생 중인 항목 강조 표시, 더블 클릭으로 재생
  - 드래그 앤 드롭으로 순서 변경, Finder에서 파일/폴더 끌어다 놓기
  - 폴더 안의 파일도 드래그 앤 드롭 또는 위로·아래로 버튼으로 순서를 바꿀 수 있습니다. *이 폴더 다시 읽기*로 새로 들어온 파일은 이름 순으로 맨 뒤에 붙고, 오른쪽 클릭 메뉴의 *이름순으로 되돌리기*로 순서를 초기화합니다.
  - 폴더 안의 파일도 선택해서 삭제(Delete / 삭제 버튼 / 오른쪽 클릭 메뉴)할 수 있습니다. 재생목록에서만 빠지고 디스크의 파일은 지워지지 않으며, 오른쪽 클릭 메뉴의 *삭제한 파일 되돌리기*로 되살립니다.
  - 이전 / 재생·일시정지 / 정지 / 다음, 탐색 슬라이더
- 반복, 무작위 재생, 소스가 숨겨졌을 때 동작(계속 재생 / 일시정지 / 정지), 단축키(재생·일시정지, 처음부터, 정지, 다음, 이전)

## 설치

[GitHub Releases](https://github.com/HAEGONG/gong2-playlist-source/releases) 페이지에서 사용하는 플랫폼에 맞는 설치 파일을 내려받습니다.

- macOS: `.pkg` (유니버설)
- Windows: 설치 프로그램 `.exe` 또는 `.zip`
- Linux: `.deb`

설치 후 OBS를 재시작하면 *소스 추가 → gong2 재생목록 소스*, *도구 → gong2 재생목록 창*을 사용할 수 있습니다. (OBS 언어가 영어면 *gong2 Playlist Source*, *gong2 Playlist Window*로 표시됩니다.)

지원 버전: OBS 33 이상 (OBS 33.0.0 기준으로 빌드).

**macOS 참고:** 패키지는 공증(notarization)되어 있지 않습니다. macOS가 설치를 막으면 `.pkg` 파일을 오른쪽 클릭 → *열기*를 선택하거나, 시스템 설정 → 개인정보 보호 및 보안에서 허용하세요.

## 소스에서 빌드 (macOS, Xcode 없이)

릴리스 패키지는 GitHub Actions로 빌드됩니다. 자세한 내용은 `.github/workflows`를 참고하세요.

아래는 로컬에서 직접 빌드하는 방법입니다. Xcode 없이 Command Line Tools + CMake + Ninja로 빌드합니다. 설치된 `/Applications/OBS.app`의 라이브러리에 직접 링크하고, 헤더는 같은 버전의 obs-studio 소스에서 가져옵니다.

```bash
brew install cmake ninja
```

```bash
scripts/build-macos.sh --install
```

처음 실행하면 `.deps/`에 다음을 내려받습니다: obs-studio 소스(설치된 OBS 버전과 동일한 태그), obs-deps의 Qt 6 및 의존성 패키지(OBS가 함께 배포하는 것과 같은 빌드). `--install`은 결과물을 `~/Library/Application Support/obs-studio/plugins/gong2-playlist-source.plugin`에 복사합니다. OBS를 재시작하면 *소스 추가 → gong2 재생목록 소스* 가 보입니다. (이전 이름 `playlist-source.plugin`이 설치돼 있으면 함께 지웁니다.)

OBS 버전이 바뀌면 `.deps/obs-studio`를 지우고 다시 빌드하세요. obs-deps 버전은 `OBS_DEPS_VERSION` 환경 변수로 바꿀 수 있습니다(해당 OBS 버전의 `CMakePresets.json` 참고).

## 구조

| 파일 | 역할 |
| --- | --- |
| `src/playlist-source.cpp` | 소스 본체. 내부 미디어 소스를 생성·제어하고 영상/오디오를 전달, 재생 순서·반복·무작위·재생 위치 관리 |
| `src/resume-store.cpp` | 재생 위치를 `resume.json`(플러그인 설정 폴더)에 기록 |
| `src/playlist-window.cpp` | Qt 재생목록 창(OBS 독) |
| `src/playlist-source.hpp` | 소스 ↔ 창 사이 API |

재생목록은 소스 설정의 `playlist` 항목(OBS 편집 목록 형식)에 저장되므로 속성 창과 재생목록 창이 같은 데이터를 편집합니다.

## 라이선스

직접 작성한 프로젝트 코드는 [0BSD](LICENSE)로 제공합니다. 출처 표기 의무 없이 사용·수정·상업적 이용·재배포할 수 있습니다. 외부 빌드 파일의 기존 라이선스는 유지되며, OBS와 링크한 플러그인을 배포할 때는 GPL 조건을 준수해야 합니다. 자세한 내용은 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)를 참고하세요.
