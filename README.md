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
```

---

## 📌 프로젝트 개요

| 항목 | 내용 |
|---|---|
| 주제 | 자동 출결 시스템 프로토콜 |
| 통신 모듈 | LoRa Board |
| 팀 | 3조 |

---

## 👥 팀 구성

| 성명 | 담당 기능 |
|---|---|
| 김채연 | 통합 관리 |
| 서여진 | 학생 FSM 구현 |
| 이용은 | 학생 FSM 구현 |
| 이미지 | CU(CP) FSM 구현 |

---

## ⚙️ 주요 기능

1. 출석 시간 내에만 출석 등록 가능
2. 입실 시 자동 출석 등록
3. 출석자 간 오픈 채팅
4. 수업 종료 5분 전 미출석자 알림
5. 강의실 이탈 후 일정 시간 경과 시 미출석 처리

---

## 📊 FSM 요약

### State

| 번호 | 상태 | 설명 |
|---|---|---|
| 0 | IDLE | 대기 중, 미입장 |
| 1 | ATTENDING | 강의실 내, 출석 인정 |
| 2 | LEAVE | 강의실 이탈, breaktime 카운트 중 |

### 상태 전환 요약

| 전환 | Event | Cond | Action |
|---|---|---|---|
| 0 → 1 | 위치 감지 (안) | 출석시간 내 | 출석 등록 |
| 0 → 0 | 출석시간 종료 | - | 미출석 확정 |
| 0 → 0 | 종료 5분 전 & 미입장 | - | send(알림) |
| 1 → 2 | 위치 감지 (밖) | - | breaktime 시작 |
| 1 → 1 | 채팅 입력 | - | send(msg) |
| 1 → 0 | 출석시간 종료 | - | - |
| 2 → 1 | 위치 감지 (안) | breaktime 미초과 | breaktime 초기화 |
| 2 → 2 | 채팅 입력 | - | send(msg) |
| 2 → 0 | breaktime 초과 | - | 미출석 확정 |

> 상세 FSM 설계 → [`docs/FSM_design.md`](docs/FSM_design.md)

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
