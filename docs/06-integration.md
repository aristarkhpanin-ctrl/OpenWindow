# 6. Подключение к умному дому

## 6.1. Что нужно со стороны координатора

Zigbee-устройству нужен **координатор** — это не роутер Wi-Fi, а отдельный
USB-стик или Ethernet-шлюз. Если у вас его ещё нет:

| Вариант | Комментарий |
|---|---|
| **SONOFF ZBDongle-E** (EFR32MG21) | лучший выбор по цене/качеству, ~2 000 ₽ |
| SLZB-06 (Ethernet/PoE) | ставится вдали от компьютера, меньше помех |
| ConBee III | хорошая альтернатива |
| Встроенный в SkyConnect / Home Assistant Yellow | если уже есть — используйте |

Дальше — два пути. Оба рабочих, выбирайте под свою систему.

## 6.2. Путь A: Home Assistant + ZHA (проще)

ZHA — встроенная в Home Assistant поддержка Zigbee. Наше устройство
использует **стандартный кластер Window Covering**, поэтому ZHA распознаёт
его автоматически, без кастомных описаний.

1. Home Assistant → **Настройки** → **Устройства и службы** → **Добавить интеграцию** → **Zigbee Home Automation**.
2. Выберите свой стик, дождитесь запуска.
3. Нажмите **Добавить устройство** — сеть откроется на 4 минуты (**Permit join**).
4. Подайте питание на привод (или нажмите RESET).
5. В Serial Monitor должно появиться:
   ```
   [ZB] подключение к сети... готово
   ```
6. В HA появится новое устройство `DIY OpenWindow-pok16` и сущность
   **`cover.openwindow_pok16`**.

Всё. В интерфейсе будут кнопки ▲ ■ ▼ и ползунок позиции.

## 6.3. Путь B: Zigbee2MQTT

Z2M тоже понимает стандартный Window Covering, но для красивых имён
и иконки стоит добавить внешнее описание.

1. Откройте Z2M → **Settings** → **Permit join** → **All**.
2. Подайте питание на привод.
3. Устройство появится как `DIY OpenWindow-pok16`.

Если Z2M пометит его как **Unsupported**, создайте файл
`data/external_converters/openwindow.js`:

```js
const {windowCovering} = require('zigbee-herdsman-converters/lib/modernExtend');

module.exports = [{
    zigbeeModel: ['OpenWindow-pok16'],
    model: 'OpenWindow-pok16',
    vendor: 'DIY',
    description: 'Привод открывания окна на проветривание',
    extend: [windowCovering({controls: ['lift'], coverMode: true})],
}];
```

и перезапустите Zigbee2MQTT.

## 6.4. Проверка после сопряжения

Прогоните по списку, не переходя к автоматизациям:

- [ ] нажатие **▲ (открыть)** открывает окно, не закрывает;
      если наоборот — `MOTOR_INVERT 1` в `config.h`;
- [ ] нажатие **■ (стоп)** останавливает створку в середине хода;
- [ ] ползунок на 50 % ставит створку примерно посередине;
- [ ] позиция в HA обновляется **во время** движения, а не только в конце;
- [ ] после `Reload` страницы позиция сохраняется;
- [ ] придержали створку рукой → привод остановился и отъехал назад;
- [ ] выключили и включили питание → позиция не потерялась.

## 6.5. Автоматизации

### Проветривание по CO₂

Самый осмысленный сценарий, ради которого всё и делается:

```yaml
automation:
  - alias: "Проветривание по CO2"
    trigger:
      - platform: numeric_state
        entity_id: sensor.co2_spalnya
        above: 1000
        for: "00:05:00"
    condition:
      - condition: numeric_state
        entity_id: sensor.temperatura_ulica
        above: 5            # не морозим квартиру
    action:
      - service: cover.set_cover_position
        target: {entity_id: cover.openwindow_pok16}
        data: {position: 40}

  - alias: "Закрыть после проветривания"
    trigger:
      - platform: numeric_state
        entity_id: sensor.co2_spalnya
        below: 700
    action:
      - service: cover.close_cover
        target: {entity_id: cover.openwindow_pok16}
```

### Аварийное закрытие

Обязательный сценарий. Окно, открытое во время дождя или отъезда, —
это испорченный пол и мокрый подоконник.

```yaml
  - alias: "Закрыть окно при дожде или уходе"
    trigger:
      - platform: state
        entity_id: binary_sensor.datchik_dozhdya
        to: "on"
      - platform: state
        entity_id: group.family
        to: "not_home"
      - platform: numeric_state
        entity_id: sensor.skorost_vetra
        above: 8
    action:
      - service: cover.close_cover
        target: {entity_id: cover.openwindow_pok16}
```

### Проветривание по расписанию

```yaml
  - alias: "Утреннее проветривание"
    trigger:
      - platform: time
        at: "07:30:00"
    condition:
      - condition: numeric_state
        entity_id: sensor.temperatura_ulica
        above: 0
    action:
      - service: cover.set_cover_position
        target: {entity_id: cover.openwindow_pok16}
        data: {position: 60}
      - delay: "00:15:00"
      - service: cover.close_cover
        target: {entity_id: cover.openwindow_pok16}
```

## 6.6. Диагностика сети

| Симптом | Причина | Решение |
|---|---|---|
| Устройство не находится | сеть не открыта | Permit join на 4 минуты |
| Находится, но сразу отваливается | помехи от проводов мотора | скрутить провода, отодвинуть от антенны |
| Долгий отклик, «залипание» | далеко от координатора | добавить Zigbee-роутер (любая розетка) между ними |
| Отваливается при работе мотора | просадка питания | конденсатор 470 мкФ на `VM` драйвера |
| Пропадает после перезагрузки HA | устройство ушло из сети | зажать BOOT 10 с, спарить заново |

**Про Wi-Fi.** Zigbee и Wi-Fi делят диапазон 2,4 ГГц. Если сеть работает
нестабильно, разведите каналы: Wi-Fi на канал 1, Zigbee на 25 или 26.
Это настраивается в ZHA/Z2M и в роутере.

## 6.7. Куда развивать дальше

1. **Мотор с энкодером** — точная позиция вместо расчёта по времени.
2. **Датчик SHT40** на I²C и второй Zigbee-эндпоинт `ZigbeeTempSensor` —
   привод сам сообщает температуру и влажность в комнате.
3. **Геркон** на створке — независимое подтверждение «окно закрыто»
   как отдельный `ZigbeeContactSwitch`.
4. **OTA-обновление** — `zbCover.addOTAClient(...)`, прошивка по воздуху
   без снятия привода с окна.
5. **Второе окно** — вся конструкция масштабируется, координатор один.
