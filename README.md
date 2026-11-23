# HERMOS CPU 백엔드

이 디렉터리에는 [Agnocast CPU IPC 백엔드](https://github.com/autowarefoundation/agnocast)를 포크한 HERMOS 소스가 있습니다. 상위 [HERMOS 워크스페이스](../README.md)에서 사용하는 공유 힙, C++ 발행/구독 라이브러리, 커널 모듈을 제공합니다. Python 및 CUDA Tensor API는 상위 워크스페이스에 있습니다.

실행에 사용되는 패키지, 라이브러리, 네임스페이스, IPC 이름은 `hermos`를 사용합니다. 기존 바이너리와 이름이 바뀐 바이너리를 혼용하면 안 되므로 전체 스택을 다시 빌드해야 합니다. 원저자 정보, 라이선스 파일, 논문 제목, 과거 변경 이력은 그대로 보존합니다.

원본 프로젝트의 설계는 아래 논문과 [프로토타입](https://github.com/sykwer/agnocast)에 설명되어 있습니다. 이 백엔드에는 [CallbackIsolatedExecutor](https://github.com/autowarefoundation/callback_isolated_executor)도 통합되어 있습니다.

- T. Ishikawa-Aso, A. Yano, T. Azumi, and S. Kato, "Work in Progress: Middleware-Transparent Callback Enforcement in Commoditized Component-Oriented Real-time Systems," in *Proc. IEEE Real-Time and Embedded Technology and Applications Symposium (RTAS)*, 2025, pp. 78–81. [IEEE Xplore](https://ieeexplore.ieee.org/document/11018762) ([pdf](https://arxiv.org/pdf/2505.06546)).
- T. Ishikawa–Aso and S. Kato, "ROS 2 Agnocast: Supporting Unsized Message Types for True Zero-Copy Publish/Subscribe IPC," in *Proc. 28th Int. Symp. Real-Time Distributed Computing (ISORC)*, 2025, pp. 1–10. [IEEE Xplore](https://ieeexplore.ieee.org/document/11173025) ([pdf](https://www.arxiv.org/pdf/2506.16882)).
- T. Ishikawa–Aso, A. Yano, K. Imai, T. Azumi, and S. Kato, "ipc_shared_ptr: A Publish/Subscribe-Aware Smart Pointer for Cross-Process Object Lifetime Management," in *Proc. 29th Int. Symp. Real-Time Distributed Computing (ISORC)*, 2026 (to appear). [arXiv:2605.04226](https://arxiv.org/abs/2605.04226) ([pdf](https://arxiv.org/pdf/2605.04226)).

<details>
<summary>BibTeX</summary>

```bibtex
@inproceedings{ishikawa2025cie,
  title={Work in Progress: Middleware-Transparent Callback Enforcement in Commoditized Component-Oriented Real-time Systems},
  author={Ishikawa-Aso, Takahiro and Yano, Atsushi and Azumi, Takuya and Kato, Shinpei},
  booktitle={2025 IEEE Real-Time and Embedded Technology and Applications Symposium (RTAS)},
  pages={78--81},
  year={2025},
  organization={IEEE}
}

@inproceedings{ishikawa2025agnocast,
  title={ROS 2 Agnocast: Supporting Unsized Message Types for True Zero-Copy Publish/Subscribe IPC},
  author={Ishikawa-Aso, Takahiro and Kato, Shinpei},
  booktitle={2025 28th International Symposium on Real-Time Distributed Computing (ISORC)},
  pages={01--10},
  year={2025},
  organization={IEEE}
}

@inproceedings{ishikawa2026agnocast,
  title={ipc\_shared\_ptr: A Publish/Subscribe-Aware Smart Pointer for Cross-Process Object Lifetime Management},
  author={Ishikawa-Aso, Takahiro and Yano, Atsushi and Imai, Koichi and Azumi, Takuya and Kato, Shinpei},
  booktitle={2026 29th International Symposium on Real-Time Distributed Computing (ISORC)},
  year={2026},
  note={to appear},
  organization={IEEE}
}
```

</details>

발표 자료:

- ROSCon 2025, 싱가포르: Takahiro Ishikawa-Aso의 "Agnocast: A ROS 2-Compatible Middleware Enabling True Zero-Copy IPC for Unsized Message Types" ([영상](https://vimeo.com/1136159023))
- ROSCon JP 2025, 나고야: Takahiro Ishikawa-Aso의 "CallbackIsolatedExecutor: 二重スケジューリングを不要にする新しいExecutorとスケジューリング理論" ([일본어 영상](https://vimeo.com/1122709186), 영어 요약은 [TIER IV 기술 블로그](https://medium.com/tier-iv-tech-blog/callbackisolatedexecutor-a-novel-executor-that-eliminates-nested-scheduling-in-scheduling-theory-400b0131ed75) 참고)

## 목차

- [지원 환경](#지원-환경)
- [현재 신뢰 경계](#현재-신뢰-경계)
- [사용자 안내](#사용자-안내)
- [개발자 안내](#개발자-안내)
- [디버깅](#디버깅)
- [문제 해결](#문제-해결)
- [관련 문서](#관련-문서)

## 지원 환경

아래 표는 원본 백엔드에서 지원하던 환경 범위입니다. 이름을 바꾼 HERMOS 스택이 모든 조합에서 검증되었다는 의미는 아닙니다. 현재 워크스페이스의 검증 결과와 GPU 관련 제약은 [워크스페이스 README](../README.md)를 참고하세요.

| 구분 | 지원 버전 / 참고 사항 |
|------|----------------------|
| ROS 2 | Humble / Jazzy (`rclcpp` 클라이언트 라이브러리만 해당) |
| Linux 배포판 | Ubuntu 22.04 (Jammy) / Ubuntu 24.04 (Noble) |
| Linux 커널 | 5.x / 6.x 계열 (세부 버전별 지원 표는 아직 없음) |

---

## 현재 신뢰 경계

상위 runtime은 참여 프로세스가 서로 신뢰하는 구성을 전제로 합니다. 현재 backend의
검사 범위도 이 전제에서 읽어야 합니다. 아래 검사는 모든 로컬 사용자 사이의 보안
격리나 메시지 기밀성을 보장하지 않습니다.

| 경로 | 현재 강제하는 조건 | 남은 경계 |
|---|---|---|
| publish·receive·take·release·remove ioctl | endpoint 소유 프로세스와 등록 effective UID 검사 | 숫자 PID 재사용에 대한 세대 검사는 없음 |
| endpoint·bridge 등록 | 현재 kmod는 IPC namespace마다 동시에 하나의 effective UID만 허용 | 같은 UID의 참여자와 daemon은 서로 신뢰 |
| bridge manager abstract UDS | kernel이 전달한 peer UID를 listener의 effective UID와 대조하고 송신 PID의 IPC namespace를 확인 | 주소는 IPC namespace inode와 수치 ROS domain으로 구분하며 도달 범위는 network namespace |
| configurator non-ROS UDS | kernel이 전달한 peer UID가 configurator의 effective UID와 같고 알린 tid가 송신 프로세스 소속 | 같은 UID의 참여자가 자기 스레드의 RT 정책을 요청할 수 있음 |
| configurator DDS·재적용 | 설정 이름과 숫자 tid | 송신자·저장된 tid 소유자를 인증하지 않음 |
| CPU 공유 힙·구독 알림 | SHM 0600 생성 후 0400, 구독자별 알림 fd, 호환 MQ는 0600 | 같은 UID의 프로세스가 신뢰 경계 안에 있음 |

장치 파일 접근 권한은 참여 제한이며 위 채널 전체의 인증을 대신하지 않습니다.
현재 빌드는 `hermos` 장치를 0666, `hermos_rclpy` 장치를 0660으로 만듭니다.
구형 kmod와의 MQ 호환은 세대 token을 발급하는 빌드에서 지원하며 같은 UID 사용은
배포 환경에서 보장해야 합니다. 구독자별 동작은 [알림 규약](docs/message_queue.md)을 참고하세요.
private IPC/mount namespace만으로 abstract UDS까지 격리되지는 않으므로, 같은 network
namespace의 프로세스도 신뢰 범위에 포함해야 합니다. 다른 사용자를 격리하는 배포에는
별도의 통신 인증과 RT 권한 부여 정책이 필요합니다.
자세한 configurator 제한은 [패키지 문서](src/hermos_cie_thread_configurator/README.md)를
참고하세요. 이 설명은 현재 구현의 한계를 기록하며 미구현 인증을 보장하지 않습니다.


## 사용자 안내

HERMOS 사용 방법은 [워크스페이스 안내](../README.md)를 따르세요. [원본 프로젝트의 시작 가이드](https://autowarefoundation.github.io/agnocast_doc/environment-setup/)는 기존 Agnocast 패키지를 설명합니다. 해당 가이드의 릴리스 바이너리와 실행 시 사용하는 이름을 이 포크에 그대로 적용할 수는 없습니다.

---

## 개발자 안내

### 백엔드 디렉터리로 이동

이 워크스페이스와 함께 제공되는 백엔드 소스를 사용하세요.

```bash
cd /path/to/rclpy_zerocopy_ws/hermos
```

원본 저장소만 복제하면 HERMOS 변경사항은 포함되지 않습니다. 원본에서 가져온 Debian 릴리스 스크립트와 과거 변경 이력만으로는 HERMOS 배포 절차가 구성되지 않습니다.

### 환경 설정

설정 스크립트를 실행해 의존성을 설치하세요.

```bash
bash scripts/dev/setup.bash
```

### pre-commit 설정

다음 명령으로 커밋 전에 `clang-format`, `markdownlint`, [KUnit 테스트](./hermos_kmod/hermos_kunit_main.c)를 실행하도록 설정할 수 있습니다.

```bash
python3 -m pip install pre-commit
python3 -m pip install --upgrade pre-commit identify
pre-commit install
```

pre-commit을 비활성화하려면 `pre-commit uninstall`을 실행하세요.

### 빌드 및 커널 모듈 로드

전체 스택을 빌드하세요.

```bash
bash scripts/dev/build_all.bash
```

`install/hermoslib/lib`에 `libhermos_heaphook.so`가 있는지 확인하세요.

```bash
$ ls install/hermoslib/lib | grep libhermos_heaphook
libhermos_heaphook.so
```

커널 모듈을 로드하세요.

```bash
cd hermos_kmod
sudo insmod hermos.ko
sudo lsmod
```

### 테스트

다음 스크립트로 빌드, 테스트, 커버리지 보고서 생성을 실행할 수 있습니다.

```bash
bash scripts/test/test_and_create_report.bash
```

### 커널 모듈 테스트

KUnit 테스트를 실행하고 커버리지 보고서를 생성하려면 다음 설정을 활성화한 사용자 지정 커널이 필요합니다. [예제 커널](https://drive.google.com/drive/folders/1sd8ROusgxhnEDOO0hbze3F5y47qtIdcM?usp=drive_link)도 제공됩니다.

- `CONFIG_KUNIT=y`
- `CONFIG_KUNIT_DEBUGFS=y`
- `CONFIG_GCOV_KERNEL=y`

해당 커널로 부팅하고 debugfs를 `/sys/kernel/debug`에 마운트한 뒤 다음 스크립트로
커널 모듈 단위 테스트를 실행하고 커버리지 보고서를 생성할 수 있습니다.
스크립트는 모듈을 내리기 전에 해당 실행의 `hermos_test_suite/results` KTAP을 읽습니다.
이미 같은 suite 결과가 있으면 실행을 거부하므로 이전 테스트 모듈이 남아 있는지 확인하세요.

```bash
bash scripts/test/run_kunit.bash
```

[pre-commit](#pre-commit-설정)을 사용할 수도 있습니다.

---

## 디버깅

커널 로그를 확인하세요.

```bash
sudo dmesg -w
```

`dynamic_debug`로 실행 중 디버그 로그 출력을 활성화하려면 관리자 권한으로 다음 명령을 실행하세요.

```bash
sudo su
echo 'file hermos_main.c +p' > /sys/kernel/debug/dynamic_debug/control
```

다음 명령으로 `dynamic_debug` 활성화 여부를 확인할 수 있습니다. `=` 오른쪽이 `p`이면 활성화, `_`이면 비활성화 상태입니다.

```bash
sudo cat /sys/kernel/debug/dynamic_debug/control | grep "hermos_main.c"
/.../hermos/hermos_kmod/hermos_main.c:810 [hermos]release_msgs_to_meet_depth =p "Release oldest message in the publisher_queue (publisher_pid=%d) of the topic (topic_name=%s) with qos_depth %d. (release_msgs_to_meet_depth)\012"
/.../hermos/hermos_kmod/hermos_main.c:367 [hermos]insert_message_entry =p "Insert an entry (topic_name=%s publisher_pid=%d msg_virtual_address=%lld timestamp=%lld). (insert_message_entry)"
```

`dynamic_debug`를 사용하려면 Linux 커널의 `CONFIG_DYNAMIC_DEBUG` 설정이 `y`여야 합니다.
현재 환경에서 이 설정이 활성화되어 있지 않다면 다음 명령으로 디버그 빌드를 수행하세요.

```bash
make CFLAGS_hermos.o="-DDEBUG"
```

컴파일 플래그에 관한 자세한 내용은 [Linux 커널의 kbuild 문서](https://www.kernel.org/doc/Documentation/kbuild/makefiles.txt)를 참고하세요.

## 문제 해결

### 공유 메모리와 메시지 큐 정리

HERMOS는 첫 번째 HERMOS 프로세스에서 fork한 백그라운드 데몬을 실행합니다. 이 데몬은 프로세스가 종료될 때 공유 메모리와 메시지 큐를 자동으로 정리합니다. 데몬은 부모 프로세스의 이름을 물려받으므로, `killall`이나 `kill -9 $(pgrep -f ...)`처럼 여러 프로세스를 한꺼번에 종료하는 명령을 쓰면 애플리케이션과 함께 종료될 수 있습니다. 데몬이 종료되면 정리 작업이 멈추고 자원이 누수됩니다. 이를 방지하려면 `Ctrl+C`를 사용하거나 특정 PID를 지정해 애플리케이션 프로세스를 개별적으로 종료하세요.

공유 메모리나 메시지 큐가 남아 있다면 수동으로 제거할 수 있습니다.

```bash
# 남아 있는 공유 메모리 제거
rm /dev/shm/hermos@*

# 남아 있는 메시지 큐 제거
rm /dev/mqueue/hermos@*
```

`mq_open failed: No space left on device` 오류는 시스템의 메시지 큐 수가 최대치에 도달했음을 뜻합니다. 위 정리 명령을 실행한 뒤에도 오류가 계속되면 시스템 전체의 `queues_max` 한도를 늘리세요(예: `sudo sysctl -w fs.mqueue.queues_max=1024`). 자세한 내용은 [시스템 설정 문서](https://autowarefoundation.github.io/agnocast_doc/environment-setup/configuration/)를 참고하세요.

## 관련 문서

- [공유 메모리](./docs/shared_memory.md)
- [메시지 큐](./docs/message_queue.md)
- [Autoware 통합](./docs/autoware_integration.md)
- [heaphook 메모리 형식](./docs/heaphook_alignment.md)
- [Clang-tidy 경고 억제](./docs/clang_tidy_suppression.md)
- [환경변수](https://autowarefoundation.github.io/agnocast_doc/api/environment-variables/)
- [ros2 명령 확장](./docs/ros2_command_extension.md)
- [hermos::Node와 rclcpp::Node 인터페이스 비교](./docs/hermos_node_interface_comparison.md)
- [HERMOS용 Callback Isolated Executor](./docs/callback_isolated_executor_for_hermos.md)
- [HERMOS-ROS 2 브리지](./docs/hermos_ros2_bridge.md)
- [Message Filters 설계 문서](./docs/message_filters_design_document.md)
- [Message Filters 마이그레이션 가이드](https://autowarefoundation.github.io/agnocast_doc/migration-guide/message-filters/)
- [ApproximateTime 알고리즘](./docs/approximate_time_algorithm.md)
