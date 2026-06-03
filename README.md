# 🎓 Auto-Attendance Protocol (AAP)

> 네트워크와 프로토콜 설계 캡스톤디자인 3조  
> LoRa 통신 모듈 기반 자동 출결 시스템 프로토콜 설계 및 구현

---

## 📁 Repository Structure

```
auto-attendance-protocol/
│
├── README.md                        # 프로젝트 개요 (현재 파일)
│
├── docs/
│   ├── specification.md             # Protocol Specification 문서
│   └── FSM_design.md                # FSM 설계 (State, Event, Action, Cond 정의)
│
└── baseCode_Capstone/               # 구현 코드
    ├── main.cpp                     # 진입점, 노드 ID 설정
    ├── protocol_parameters.h        # 전체 파라미터 상수 정의
    ├── L2_FSMmain.cpp/h             # L2 ARQ 상태 기계
    ├── L2_msg.cpp/h                 # L2 PDU 인코딩/디코딩
    ├── L2_LLinterface.cpp/h         # L2 ↔ PHYMAC 인터페이스
    ├── L3_FSMmain_CU.cpp            # CU L3 FSM
    ├── L3_FSMmain_student.cpp       # 학생 L3 FSM
    ├── L3_LLinterface.cpp/h         # L3 ↔ L2 인터페이스
    ├── L3_chatProtocol.h            # 채팅 패킷 구조체 및 생성 함수
    ├── L3_convertPacket.h           # 출석 패킷 구조체 및 생성 함수
    └── Makefile                     # CU / student 분리 빌드
```

---

## 📌 프로젝트 개요

| 항목 | 내용 |
|---|---|
| 주제 | 자동 출결 시스템 프로토콜 |
| 통신 모듈 | LoRa Board (mbed) |
| 팀 | 3조 |

---

## 👥 팀 구성

| 성명 | 담당 기능 |
|---|---|
| 김채연 | 통합 관리 |
| 서여진 | 학생 FSM 구현 |
| 이용은 | 학생 FSM 구현 |
| 이미지 | CU FSM 구현 |

---

## ⚙️ 주요 기능

1. 출석 시간 내에만 출석 등록 가능 (RSSI 기반 강의실 내 위치 판단)
2. 입실 시 자동 출석 등록 (PRESENCE 신호 → CU RSSI 평균 측정 → 승인)
3. 출석 확인된 학생 간 1:1 채팅 (`<상대ID> <메시지>` 입력)
4. 마감 전 미출석자 알림 (마감 `L3_PRE_DEADLINE_ALERT_SEC`초 전 브로드캐스트)
5. 강의실 이탈 감지 (RSSI 임계값 미만 → LEAVE 전이, 유예 시간 초과 시 결석 확정)
6. 이탈 후 복귀 감지 (RSSI 회복 시 ATTEND 복귀)
7. 출석 창 종료 후 재오픈 가능 (`start` 재입력 → 모든 학생 상태 초기화 후 재시작)
8. 부팅 시 노드 번호 입력 (`Enter node number (1-99):`, 학생 단말 전용)

---

## 🏗️ 빌드 방법

```bash
# CU (교수자 단말) 빌드
make NODE_TYPE=CU

# 학생 단말 빌드
make NODE_TYPE=student
```

- **CU**: ID = 0 고정, 브로드캐스트 송신
- **학생**: 부팅 시 `Enter node number (1-99):` 프롬프트로 ID 직접 입력

---

## 💬 채팅 사용법

출석 승인(ATTEND 상태) 후 사용 가능.

```
[CHAT FORMAT] <destId> <message>
example: 3 hello
> 3 안녕하세요

[MSG] TO: 3 | CONTENT: 안녕하세요       ← 송신 측 출력
[MSG] FROM: 1 | CONTENT: 안녕하세요     ← 수신 측 출력
```

> CU ↔ 학생 채팅은 미지원. 학생 ↔ 학생 간 1:1 채팅만 가능.

---

## 📊 FSM 요약

### 학생 단말 FSM

| 번호 | 상태 | 설명 |
|---|---|---|
| 0 | IDLE | 대기 중, 출석 창 열리길 기다림 |
| 1 | ATTEND | 강의실 내, 출석 인정, 채팅 가능 |
| 2 | LEAVE | 강의실 이탈, 유예 타이머 진행 중 |

#### 상태 전환

| 전환 | 이벤트 / 조건 | 동작 |
|---|---|---|
| IDLE → ATTEND | CU 승인 수신 (RSSI ≥ 임계값) | 출석 등록 |
| IDLE → IDLE | 출석 창 OPEN 수신 | PRESENCE 자동 전송 |
| IDLE → IDLE | 마감 경고 수신 | 경고 출력 |
| IDLE → IDLE | 출석 창 CLOSED 수신 | 결석 확정 |
| ATTEND → LEAVE | CU 이탈 감지 신호 수신 (ok=0) | 유예 타이머 시작 |
| ATTEND → IDLE | 출석 창 CLOSED 수신 | 세션 종료 |
| LEAVE → ATTEND | CU 복귀 승인 수신 (ok=1) | 유예 타이머 초기화 |
| LEAVE → IDLE | 출석 창 CLOSED 수신 | 결석 확정 |

---

### CU FSM

| 번호 | 상태 | 설명 |
|---|---|---|
| 0 | WAIT | `start` 명령 대기 |
| 1 | OPEN | 출석 창 활성화, PRESENCE 수신 처리 중 |
| 2 | CLOSED | 출석 창 종료, `start` 입력으로 재오픈 가능 |

#### 상태 전환

| 전환 | 이벤트 / 조건 | 동작 |
|---|---|---|
| WAIT → OPEN | `start` 입력 | OPEN 브로드캐스트, 타이머 시작 |
| OPEN → OPEN | PRESENCE 수신 (RSSI ≥ 임계값) | 출석 승인 전송 (IDLE→ATTEND) |
| OPEN → OPEN | PRESENCE 수신 (RSSI < 임계값, 미출석) | 거부 전송 |
| OPEN → OPEN | PRESENCE 수신 (RSSI < 임계값, 출석 후) | 이탈 감지, ok=0 전송 (ATTEND→LEAVE) |
| OPEN → OPEN | 이탈 중 RSSI 회복 | ok=1 전송 (LEAVE→ATTEND) |
| OPEN → OPEN | 이탈 유예 시간 초과 | 결석 확정, attendanceTable 초기화 |
| OPEN → CLOSED | 마감 타이머 만료 | CLOSED 브로드캐스트, 출석 요약 출력 |
| CLOSED → OPEN | `start` 재입력 | 전체 학생 상태 초기화 후 재오픈 |

> 상세 FSM 설계 → [`docs/FSM_design.md`](docs/FSM_design.md)

---

## 🔧 프로토콜 파라미터

`baseCode_Capstone/protocol_parameters.h` 에서 조정 가능.

| 파라미터 | 기본값 | 설명 |
|---|---|---|
| `L3_RSSI_THRESHOLD` | -50 dBm | 출석 인정 RSSI 임계값 |
| `L3_ATTEND_WINDOW_SEC` | 60 s | 출석 창 유지 시간 |
| `L3_PRE_DEADLINE_ALERT_SEC` | 30 s | 마감 전 경고 시점 |
| `L3_LEAVE_GRACE_SEC` | 30 s | 이탈 유예 시간 |
| `L3_MAX_STUDENTS` | 32 | 최대 학생 수 (ID 범위: 1 ~ 31) |
| `L3_RSSI_SAMPLE_COUNT` | 3 | 출석 판단용 RSSI 평균 샘플 수 |
| `L2_ARQ_MAXRETRANSMISSION` | 10 | ARQ 최대 재전송 횟수 |
| `L2_ARQ_MAXWAITTIME` | 5 s | ACK 최대 대기 시간 |

---

## 📅 수행 일정

| 주차 | 수행 내용 |
|---|---|
| 1 | System Specification 구상 및 설계 계획 |
| 2 | 입장 시 자동 출석 시스템 구현 |
| 3 | 이탈 감지 및 미출석 전환 기능 구현 |
| 4 | 오픈 채팅 시스템 구현 |
| 5 | 미출석자 대상 알림 기능 구현 |
| 6 | 기능 통합 및 전체 시스템 완성, 발표 영상 준비 |
