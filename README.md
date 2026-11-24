# Nearic Firmware

Прошивка Nearic управляет звуком, экраном, сенсорным вводом и обновлениями устройства. Первая аппаратная платформа — ESP32-S3-BOX-3B.

Nearic Service связывает устройство с выбранным разговором пользователя. Обмен описан в [контракте подключения устройства](specs/001-device-connection/contracts/device-service.md). Общий коннектор, плагин Codex и CLI ведутся в [Nearic Connectors](https://github.com/Hireminds/nearic-connectors).

## Состояние

Первая прошивка в разработке. Проект ESP-IDF находится в `firmware/`.
[Сценарий проверки](specs/001-device-connection/quickstart.md) описывает сборку
и приёмку на BOX. Для знакомства с платой доступны
[характеристики и заводские примеры Espressif](docs/hardware/BOX-3B.md).

## Сборка

Запустите команды из корня репозитория в активированной среде ESP-IDF v5.5.6.

```bash
idf.py -C firmware set-target esp32s3
idf.py -C firmware build
```

Образ появится в `firmware/build/nearic.bin`. Для установки и проверки OTA используйте
[инструкцию по подписанию и восстановлению](specs/001-device-connection/quickstart.md).

## Техническая документация

- [Требования к прошивке](docs/PRODUCT.md)
- [Архитектура](docs/architecture/README.md) и [граница с сервисом](docs/contracts/README.md)
- [Сценарии устройства](docs/flows/README.md)
- [Экран и дизайн-система](docs/design-system/README.md)
- [Оборудование](docs/hardware/BOX-3B.md) и [проверки](docs/process/testing.md)
- [Доменные спецификации](specs/README.md) и [решения](docs/decisions/README.md)

## Разработка

Откройте репозиторий как проект Codex. Начните с [руководства участника](CONTRIBUTING.md) и [карты процесса Spec Kit](docs/process/spec-kit.md). Правила агента находятся в [AGENTS.md](AGENTS.md), принципы — в [конституции](.specify/memory/constitution.md).
