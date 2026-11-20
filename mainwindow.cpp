#include "mainwindow.h"

#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QFrame>
#include <QStackedWidget>
#include <QLineEdit>
#include <QSpinBox>
#include <QPushButton>
#include <QLabel>
#include <QSlider>
#include <QCheckBox>
#include <QPlainTextEdit>
#include <QStyle>
#include <QJsonDocument>
#include <QJsonValue>
#include <QDateTime>
#include <QChart>
#include <QChartView>
#include <QLineSeries>
#include <QValueAxis>

#include <QTabWidget>
#include <QScrollArea>
#include <QTableWidget>
#include <QHeaderView>
#include <QStandardPaths>
#include <QDir>
#include <QFile>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlError>

// Contrato con el firmware de la Práctica 2 (practica4.ino de 2026A, puerto 81):
//   Qt -> ESP32  {"tipo":"latido"}
//                {"tipo":"mover","accion":"avanzar|retroceder|izquierda|derecha|parar","ms":400}
//                {"tipo":"servo","angulo":0..180}
//                {"tipo":"leer_distancia"}
//                {"tipo":"scan","angulo":0..180}
//   ESP32 -> Qt  saludo, latido_ok, mover_ack / mover_err, mover_inicio, mover_fin,
//                {"tipo":"distancia_lectura","d":cm}, {"tipo":"scan","d":cm,"angulo":n}, telemetria
// El firmware reemplaza el movimiento en curso cuando llega una orden nueva, así que
// repetir "mover" mientras el dedo está en el botón no acumula órdenes.

static const int MS_MOVIMIENTO = 400;   // duración de cada orden "mover"
static const int MS_REPETIR = 200;      // cada cuánto se reenvía mientras se presiona
static const int VENTANA_MUESTRAS = 40; // puntos visibles en la gráfica

// Regla antiplagio de la Práctica 3: escribe aquí tu nombre y tu código.
// Debe verse en todas tus capturas del teléfono.
static const char *ALUMNO = "Fatima Ochoa · 220461691";

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    construirInterfaz();

    m_timerAutoTemp.setInterval(10000); // 10 s como pide la guía en E3
    connect(&m_timerAutoTemp, &QTimer::timeout, this, &MainWindow::solicitarTemperatura);

    inicializarBaseDatos();

    connect(&m_socket, &QWebSocket::connected, this, &MainWindow::alConectar);
    connect(&m_socket, &QWebSocket::disconnected, this, &MainWindow::alDesconectar);
    connect(&m_socket, &QWebSocket::textMessageReceived, this, &MainWindow::alRecibirMensaje);
    connect(&m_socket, &QWebSocket::errorOccurred, this, &MainWindow::alOcurrirError);

    m_latido.setInterval(1000);
    connect(&m_latido, &QTimer::timeout, this, [this] { enviar({{"tipo", "latido"}}); });

    m_repetir.setInterval(MS_REPETIR);
    connect(&m_repetir, &QTimer::timeout, this, &MainWindow::repetirMovimiento);

    m_servo.setInterval(60);
    m_servo.setSingleShot(true);
    connect(&m_servo, &QTimer::timeout, this, &MainWindow::enviarServoPendiente);

    m_medirAuto.setInterval(500);
    connect(&m_medirAuto, &QTimer::timeout, this, &MainWindow::alPresionarMedir);

    m_barrido.setInterval(700);   // el firmware espera 500 ms antes de medir en cada "scan"
    connect(&m_barrido, &QTimer::timeout, this, &MainWindow::siguientePasoBarrido);
}

// ---------------------------------------------------------------- interfaz
void MainWindow::construirInterfaz()
{
    auto *central = new QWidget(this);
    auto *raiz = new QVBoxLayout(central);
    raiz->setSpacing(8);

    auto *alumno = new QLabel(QString::fromUtf8(ALUMNO));
    alumno->setAlignment(Qt::AlignCenter);
    alumno->setStyleSheet("font-weight: bold; color: #1F4E79;");
    raiz->addWidget(alumno);

    // 1) Conexión
    auto *cajaConexion = new QGroupBox(tr("Conexión con el robot"));
    auto *filaConexion = new QHBoxLayout(cajaConexion);
    m_ip = new QLineEdit(QStringLiteral("192.168.1.101"));
    m_ip->setPlaceholderText(tr("IP de la ESP32-S3"));
    m_ip->setInputMethodHints(Qt::ImhPreferNumbers);   // teclado numérico en Android
    m_puerto = new QSpinBox;
    m_puerto->setRange(1, 65535);
    m_puerto->setValue(81);
    m_conectar = new QPushButton(tr("Conectar"));
    filaConexion->addWidget(m_ip, 3);
    filaConexion->addWidget(m_puerto, 1);
    filaConexion->addWidget(m_conectar, 2);
    connect(m_conectar, &QPushButton::clicked, this, &MainWindow::alPresionarConectar);
    m_estado = new QLabel(tr("Desconectado"));
    m_estado->setAlignment(Qt::AlignCenter);
    raiz->addWidget(cajaConexion);
    raiz->addWidget(m_estado);

    // 2) Cruceta de movimiento: botones grandes para el dedo
    auto *cajaMover = new QGroupBox(tr("Movimiento (mantén presionado)"));
    auto *cruceta = new QGridLayout(cajaMover);
    // Íconos estándar de Qt: se ven iguales en Windows, Linux y Android
    cruceta->addWidget(botonMovimiento(QStyle::SP_ArrowUp, "avanzar"), 0, 1);
    cruceta->addWidget(botonMovimiento(QStyle::SP_ArrowLeft, "izquierda"), 1, 0);
    cruceta->addWidget(botonMovimiento(QStyle::SP_MediaStop, "parar"), 1, 1);
    cruceta->addWidget(botonMovimiento(QStyle::SP_ArrowRight, "derecha"), 1, 2);
    cruceta->addWidget(botonMovimiento(QStyle::SP_ArrowDown, "retroceder"), 2, 1);
    raiz->addWidget(cajaMover);

    // 3) Servo
    auto *cajaServo = new QGroupBox(tr("Servo del sensor"));
    auto *filaServo = new QHBoxLayout(cajaServo);
    m_slider = new QSlider(Qt::Horizontal);
    m_slider->setRange(0, 180);
    m_slider->setValue(90);
    m_angulo = new QLabel(QStringLiteral("90°"));
    m_angulo->setMinimumWidth(48);
    auto *barrido = new QPushButton(tr("Barrido"));
    filaServo->addWidget(m_slider, 1);
    filaServo->addWidget(m_angulo);
    filaServo->addWidget(barrido);
    connect(m_slider, &QSlider::valueChanged, this, &MainWindow::alMoverServo);
    connect(barrido, &QPushButton::clicked, this, &MainWindow::alPresionarBarrido);
    raiz->addWidget(cajaServo);

    // 4) Distancia: lectura y gráfica
    auto *cajaDist = new QGroupBox(tr("Distancia del HC-SR04"));
    auto *colDist = new QVBoxLayout(cajaDist);
    auto *filaDist = new QHBoxLayout;
    m_distancia = new QLabel(QStringLiteral("— cm"));
    QFont grande = m_distancia->font();
    grande.setPointSize(grande.pointSize() + 6);
    grande.setBold(true);
    m_distancia->setFont(grande);
    auto *medir = new QPushButton(tr("Medir"));
    m_auto = new QCheckBox(tr("Automático"));
    filaDist->addWidget(m_distancia, 1);
    filaDist->addWidget(medir);
    filaDist->addWidget(m_auto);
    connect(medir, &QPushButton::clicked, this, &MainWindow::alPresionarMedir);
    connect(m_auto, &QCheckBox::toggled, this, [this](bool activo) {
        activo ? m_medirAuto.start() : m_medirAuto.stop();
    });
    colDist->addLayout(filaDist);

    m_serie = new QLineSeries;
    auto *grafica = new QChart;
    grafica->addSeries(m_serie);
    grafica->legend()->hide();
    m_ejeX = new QValueAxis;
    m_ejeX->setRange(0, VENTANA_MUESTRAS);
    m_ejeX->setLabelFormat("%d");
    m_ejeX->setTitleText(tr("muestra"));
    auto *ejeY = new QValueAxis;
    ejeY->setRange(0, 200);
    ejeY->setTitleText(tr("cm"));
    grafica->addAxis(m_ejeX, Qt::AlignBottom);
    grafica->addAxis(ejeY, Qt::AlignLeft);
    m_serie->attachAxis(m_ejeX);
    m_serie->attachAxis(ejeY);
    auto *vista = new QChartView(grafica);
    vista->setRenderHint(QPainter::Antialiasing);
    vista->setMinimumHeight(180);
    colDist->addWidget(vista, 1);
    raiz->addWidget(cajaDist, 1);

    // 5) Registro de mensajes
    m_registro = new QPlainTextEdit;
    m_registro->setReadOnly(true);
    m_registro->setMaximumBlockCount(200);
    m_registro->setMaximumHeight(110);
    raiz->addWidget(m_registro);

    auto *tabs = new QTabWidget(this);
    tabs->addTab(central, tr("Robot y Distancia"));
    agregarPestanaTemperatura(tabs);

    stackedWidget = new QStackedWidget(this);

    auto *pageLogin = new QWidget(stackedWidget);
    auto *gridLogin = new QGridLayout(pageLogin);

    auto *frameLogin = new QFrame(pageLogin);
    frameLogin->setStyleSheet("background-color: white; border: 1px solid #dbe3ed; border-radius: 12px;");
    frameLogin->setMaximumSize(360, 280);

    auto *vLogin = new QVBoxLayout(frameLogin);
    vLogin->setSpacing(12);
    vLogin->setContentsMargins(20, 20, 20, 20);

    auto *lblTituloLogin = new QLabel(tr("ACCESO RESTRINGIDO · %1").arg(QString::fromUtf8(ALUMNO)), frameLogin);
    lblTituloLogin->setStyleSheet("font-weight: bold; color: #1F4E79;");
    lblTituloLogin->setAlignment(Qt::AlignCenter);
    lblTituloLogin->setWordWrap(true);

    lineEditPassword = new QLineEdit(frameLogin);
    lineEditPassword->setPlaceholderText(tr("Contraseña..."));
    lineEditPassword->setEchoMode(QLineEdit::Password);

    btnEntrar = new QPushButton(tr("Ingresar"), frameLogin);
    btnEntrar->setStyleSheet("background-color: #7c3aed; color: white; font-weight: bold; padding: 10px; border-radius: 8px;");

    labelErrorPassword = new QLabel(frameLogin);
    labelErrorPassword->setStyleSheet("color: #dc2626; font-size: 12px;");
    labelErrorPassword->setAlignment(Qt::AlignCenter);

    vLogin->addWidget(lblTituloLogin);
    vLogin->addWidget(lineEditPassword);
    vLogin->addWidget(btnEntrar);
    vLogin->addWidget(labelErrorPassword);

    gridLogin->addWidget(frameLogin, 0, 1);

    stackedWidget->addWidget(pageLogin);
    stackedWidget->addWidget(tabs);
    stackedWidget->setCurrentIndex(0);
    setCentralWidget(stackedWidget);

    connect(btnEntrar, &QPushButton::clicked, this, &MainWindow::validarLogin);
    connect(lineEditPassword, &QLineEdit::returnPressed, this, &MainWindow::validarLogin);

    setWindowTitle(tr("Robot P3 · Android"));
}

QPushButton *MainWindow::botonMovimiento(QStyle::StandardPixmap icono, const QString &accion)
{
    auto *b = new QPushButton(style()->standardIcon(icono), QString());
    b->setIconSize(QSize(40, 40));
    b->setMinimumSize(72, 64);
    b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    b->setAccessibleName(accion);   // nombre para lectores de pantalla y pruebas automáticas
    // pressed/released funcionan igual con el ratón y con el dedo
    connect(b, &QPushButton::pressed, this, [this, accion] {
        if (accion == "parar") {
            m_repetir.stop();
            enviarMover("parar", 0);
            return;
        }
        m_accionActual = accion;
        enviarMover(accion, MS_MOVIMIENTO);
        m_repetir.start();
    });
    connect(b, &QPushButton::released, this, [this, accion] {
        if (accion == "parar") return;
        m_repetir.stop();
        enviarMover("parar", 0);
    });
    return b;
}

// ---------------------------------------------------------------- conexión
void MainWindow::alPresionarConectar()
{
    if (m_socket.state() != QAbstractSocket::UnconnectedState) {
        m_socket.close();
        return;
    }
    const QUrl url(QStringLiteral("ws://%1:%2").arg(m_ip->text().trimmed()).arg(m_puerto->value()));
    m_estado->setText(tr("Conectando a %1 …").arg(url.toString()));
    m_conectar->setText(tr("Cancelar"));
    m_socket.open(url);
}

void MainWindow::alConectar()
{
    m_estado->setText(tr("Conectado a %1").arg(m_socket.requestUrl().toString()));
    m_conectar->setText(tr("Desconectar"));
    m_latido.start();
    enviar({{"tipo", "latido"}});
    registrar(tr("conectado"));
}

void MainWindow::alDesconectar()
{
    m_latido.stop();
    m_repetir.stop();
    m_medirAuto.stop();
    m_barrido.stop();
    m_auto->setChecked(false);
    m_estado->setText(tr("Desconectado"));
    m_conectar->setText(tr("Conectar"));
    registrar(tr("desconectado"));
    m_timerAutoTemp.stop();
    if (m_chkAutoTemp)
        m_chkAutoTemp->setChecked(false);
}

void MainWindow::alOcurrirError(QAbstractSocket::SocketError)
{
    registrar(tr("error: %1").arg(m_socket.errorString()));
}

void MainWindow::enviar(const QJsonObject &obj)
{
    if (m_socket.state() != QAbstractSocket::ConnectedState)
        return;
    m_socket.sendTextMessage(QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact)));
}

void MainWindow::enviarMover(const QString &accion, int ms)
{
    enviar({{"tipo", "mover"}, {"accion", accion}, {"ms", ms}});
}

void MainWindow::repetirMovimiento()
{
    enviarMover(m_accionActual, MS_MOVIMIENTO);
}

// ---------------------------------------------------------------- mensajes
void MainWindow::alRecibirMensaje(const QString &texto)
{
    const QJsonObject obj = QJsonDocument::fromJson(texto.toUtf8()).object();
    const QString tipo = obj.value("tipo").toString();

    if (tipo == "dht_lectura") {
        const QJsonValue t = obj.value("temperatura");
        const bool dhtOk = obj.value("dht_ok").toBool(false);
        if (!dhtOk || !t.isDouble()) {
            registrar(tr("Lectura inválida: no se guarda."));
            return;
        }
        const double tempVal = t.toDouble();
        if (m_labelTempActual) {
            m_labelTempActual->setText(QStringLiteral("%1 °C").arg(tempVal, 0, 'f', 1));
        }
        guardarTemperatura(tempVal);
    } else if (tipo == "distancia_lectura") {
        agregarDistancia(obj.value("d").toDouble());
    } else if (tipo == "scan") {
        agregarDistancia(obj.value("d").toDouble());
        registrar(tr("scan %1° → %2 cm").arg(obj.value("angulo").toInt())
                      .arg(obj.value("d").toDouble(), 0, 'f', 1));
    } else if (tipo == "saludo") {
        registrar(tr("saludo de %1").arg(obj.value("nombre_host").toString()));
    } else if (tipo == "mover_err") {
        registrar(tr("el robot rechazó la orden %1").arg(obj.value("accion").toString()));
    } else if (tipo == "latido_ok" || tipo == "telemetria" || tipo.startsWith("mover_")) {
        // frecuentes: no se registran para no llenar la pantalla
    } else {
        registrar(texto);
    }
}

// ---------------------------------------------------------------- servo y distancia
void MainWindow::alMoverServo(int angulo)
{
    m_angulo->setText(QStringLiteral("%1°").arg(angulo));
    m_anguloPendiente = angulo;
    if (!m_servo.isActive())
        m_servo.start();   // como máximo un mensaje cada 60 ms aunque el dedo se deslice rápido
}

void MainWindow::enviarServoPendiente()
{
    if (m_anguloPendiente < 0) return;
    enviar({{"tipo", "servo"}, {"angulo", m_anguloPendiente}});
    m_anguloPendiente = -1;
}

void MainWindow::alPresionarMedir()
{
    enviar({{"tipo", "leer_distancia"}});
}

void MainWindow::alPresionarBarrido()
{
    m_anguloBarrido = 0;
    m_barrido.start();
    siguientePasoBarrido();
}

void MainWindow::siguientePasoBarrido()
{
    if (m_anguloBarrido > 180) {
        m_barrido.stop();
        return;
    }
    enviar({{"tipo", "scan"}, {"angulo", m_anguloBarrido}});
    m_slider->blockSignals(true);   // mueve el deslizador sin mandar otra orden "servo"
    m_slider->setValue(m_anguloBarrido);
    m_slider->blockSignals(false);
    m_angulo->setText(QStringLiteral("%1°").arg(m_anguloBarrido));
    m_anguloBarrido += 30;
}

void MainWindow::agregarDistancia(double cm)
{
    m_distancia->setText(QStringLiteral("%1 cm").arg(cm, 0, 'f', 1));
    m_serie->append(m_muestra, cm);
    if (m_serie->count() > VENTANA_MUESTRAS)
        m_serie->remove(0);   // ventana deslizante: el buffer no crece sin límite
    ++m_muestra;
    m_ejeX->setRange(qMax(0, m_muestra - VENTANA_MUESTRAS), qMax(VENTANA_MUESTRAS, m_muestra));
}

void MainWindow::registrar(const QString &linea)
{
    m_registro->appendPlainText(QDateTime::currentDateTime().toString("hh:mm:ss  ") + linea);
}

void MainWindow::agregarPestanaTemperatura(QTabWidget *tabs)
{
    auto *tabTemp = new QWidget;
    auto *layoutTab = new QVBoxLayout(tabTemp);
    layoutTab->setContentsMargins(0, 0, 0, 0);

    auto *scroll = new QScrollArea(tabTemp);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);

    auto *contenido = new QWidget;
    auto *lay = new QVBoxLayout(contenido);
    lay->setSpacing(10);
    lay->setContentsMargins(10, 10, 10, 10);

    auto *lblInfo = new QLabel(QStringLiteral("ALUMNO: Fatima Ochoa · 220461691 — base: temperatura_1691.db"), contenido);
    lblInfo->setStyleSheet("font-weight: bold; color: #1F4E79;");
    lblInfo->setWordWrap(true);
    lay->addWidget(lblInfo);

    m_labelTempActual = new QLabel(QStringLiteral("--.- °C"), contenido);
    QFont fTemp = m_labelTempActual->font();
    fTemp.setPointSize(fTemp.pointSize() + 8);
    fTemp.setBold(true);
    m_labelTempActual->setFont(fTemp);
    m_labelTempActual->setAlignment(Qt::AlignCenter);
    lay->addWidget(m_labelTempActual);

    auto *filaAccion = new QHBoxLayout;
    auto *btnLeer = new QPushButton(tr("Leer temperatura"), contenido);
    m_chkAutoTemp = new QCheckBox(tr("Guardar cada 10 s"), contenido);
    filaAccion->addWidget(btnLeer);
    filaAccion->addWidget(m_chkAutoTemp);
    lay->addLayout(filaAccion);

    connect(btnLeer, &QPushButton::clicked, this, &MainWindow::solicitarTemperatura);
    connect(m_chkAutoTemp, &QCheckBox::toggled, this, [this](bool activo) {
        if (activo) {
            m_timerAutoTemp.start(10000);
            solicitarTemperatura();
        } else {
            m_timerAutoTemp.stop();
        }
    });

    m_tablaHistorial = new QTableWidget(0, 4, contenido);
    m_tablaHistorial->setHorizontalHeaderLabels({tr("ID"), tr("Fecha/Hora"), tr("Temp (°C)"), tr("Nota")});
    m_tablaHistorial->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_tablaHistorial->setMinimumHeight(180);
    m_tablaHistorial->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    lay->addWidget(m_tablaHistorial);

    auto *filaTotal = new QHBoxLayout;
    auto *btnCargar = new QPushButton(tr("Cargar historial"), contenido);
    m_labelTotalTemp = new QLabel(tr("Mediciones guardadas: 0"), contenido);
    filaTotal->addWidget(btnCargar);
    filaTotal->addWidget(m_labelTotalTemp);
    lay->addLayout(filaTotal);
    connect(btnCargar, &QPushButton::clicked, this, &MainWindow::cargarHistorial);

    auto *filaNota = new QHBoxLayout;
    m_spinIdTemp = new QSpinBox(contenido);
    m_spinIdTemp->setRange(0, 99999);
    m_lineNotaTemp = new QLineEdit(contenido);
    m_lineNotaTemp->setPlaceholderText(tr("Nota para el ID seleccionado..."));
    auto *btnNota = new QPushButton(tr("Guardar nota"), contenido);
    filaNota->addWidget(new QLabel(tr("ID:"), contenido));
    filaNota->addWidget(m_spinIdTemp);
    filaNota->addWidget(m_lineNotaTemp, 1);
    filaNota->addWidget(btnNota);
    lay->addLayout(filaNota);

    connect(btnNota, &QPushButton::clicked, this, &MainWindow::guardarNota);
    connect(m_tablaHistorial, &QTableWidget::cellClicked, this, [this](int fila, int) {
        if (m_tablaHistorial->item(fila, 0))
            m_spinIdTemp->setValue(m_tablaHistorial->item(fila, 0)->text().toInt());
    });

    scroll->setWidget(contenido);
    layoutTab->addWidget(scroll);
    tabs->addTab(tabTemp, tr("Temperatura"));
}

void MainWindow::inicializarBaseDatos()
{
    const QString carpeta = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(carpeta);

    const QString ruta = carpeta + "/temperatura_1691.db";
    const bool yaExistia = QFile::exists(ruta);

    QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE");
    db.setDatabaseName(ruta);

    if (!db.open()) {
        registrar(tr("No se abrió la base: %1").arg(db.lastError().text()));
        return;
    }

    QSqlQuery consulta;
    if (!consulta.exec("CREATE TABLE IF NOT EXISTS mediciones ("
                       "id INTEGER PRIMARY KEY AUTOINCREMENT, "
                       "fecha_hora TEXT NOT NULL, "
                       "temperatura REAL NOT NULL, "
                       "fuente TEXT NOT NULL, "
                       "nota TEXT)")) {
        registrar(tr("No se creó la tabla: %1").arg(consulta.lastError().text()));
        return;
    }

    if (!consulta.exec("CREATE TABLE IF NOT EXISTS accesos ("
                       "id INTEGER PRIMARY KEY AUTOINCREMENT, "
                       "fecha_hora TEXT NOT NULL, "
                       "estado TEXT NOT NULL)")) {
        registrar(tr("No se creó la tabla accesos: %1").arg(consulta.lastError().text()));
    }

    registrar((yaExistia ? tr("Base encontrada: ") : tr("Base nueva creada: ")) + ruta);
    cargarHistorial();
}

void MainWindow::solicitarTemperatura()
{
    if (m_socket.state() != QAbstractSocket::ConnectedState)
        return;
    enviar({{"tipo", "leer_dht"}});
}

void MainWindow::guardarTemperatura(double celsius)
{
    static const char *FUENTE = "simulada"; // corresponde a SIMULAR_DHT = true en tu firmware
    const double c1 = qRound(celsius * 10.0) / 10.0;

    QSqlQuery consulta;
    consulta.prepare("INSERT INTO mediciones (fecha_hora, temperatura, fuente) "
                     "VALUES (:fecha_hora, :temperatura, :fuente)");
    consulta.bindValue(":fecha_hora", QDateTime::currentDateTime().toString(Qt::ISODate));
    consulta.bindValue(":temperatura", c1);
    consulta.bindValue(":fuente", QString::fromUtf8(FUENTE));

    if (!consulta.exec()) {
        registrar(tr("INSERT falló: %1").arg(consulta.lastError().text()));
        return;
    }

    registrar(tr("Guardada %1 °C con id %2")
                  .arg(c1, 0, 'f', 1)
                  .arg(consulta.lastInsertId().toString()));
    cargarHistorial();
}

void MainWindow::cargarHistorial()
{
    if (!m_tablaHistorial) return;

    m_tablaHistorial->setEditTriggers(QAbstractItemView::NoEditTriggers);
    QSqlQuery consulta("SELECT id, fecha_hora, temperatura, nota FROM mediciones ORDER BY id DESC LIMIT 50");
    if (!consulta.isActive()) {
        registrar(tr("SELECT falló: %1").arg(consulta.lastError().text()));
        return;
    }

    m_tablaHistorial->setRowCount(0);
    while (consulta.next()) {
        const int fila = m_tablaHistorial->rowCount();
        m_tablaHistorial->insertRow(fila);
        m_tablaHistorial->setItem(fila, 0, new QTableWidgetItem(consulta.value(0).toString()));
        m_tablaHistorial->setItem(fila, 1, new QTableWidgetItem(consulta.value(1).toString()));
        m_tablaHistorial->setItem(fila, 2, new QTableWidgetItem(QString::number(consulta.value(2).toDouble(), 'f', 1)));
        m_tablaHistorial->setItem(fila, 3, new QTableWidgetItem(consulta.value(3).toString()));
    }

    QSqlQuery total("SELECT COUNT(*) FROM mediciones");
    if (total.next() && m_labelTotalTemp) {
        m_labelTotalTemp->setText(tr("Mediciones guardadas: %1").arg(total.value(0).toString()));
    }
}

void MainWindow::guardarNota()
{
    if (!m_spinIdTemp || !m_lineNotaTemp) return;
    const int idTarget = m_spinIdTemp->value();
    if (idTarget == 0) {
        registrar(tr("Primero selecciona un ID válido."));
        return;
    }

    QSqlQuery consulta;
    consulta.prepare("UPDATE mediciones SET nota = :nota WHERE id = :id");
    consulta.bindValue(":nota", m_lineNotaTemp->text());
    consulta.bindValue(":id", idTarget);

    if (!consulta.exec()) {
        registrar(tr("UPDATE falló: %1").arg(consulta.lastError().text()));
        return;
    }

    const int afectadas = consulta.numRowsAffected();
    registrar(tr("UPDATE id %1. Filas cambiadas: %2").arg(idTarget).arg(afectadas));
    cargarHistorial();
}

void MainWindow::validarLogin()
{
    const QString password = lineEditPassword->text();
    const bool ok = (password == "micarrito123");

    // Registrar intento en SQLite (temperatura_1691.db)
    QSqlQuery q;
    q.prepare("INSERT INTO accesos (fecha_hora, estado) VALUES (:fh, :est)");
    q.bindValue(":fh", QDateTime::currentDateTime().toString(Qt::ISODate));
    q.bindValue(":est", ok ? "ACCESO_CONCEDIDO" : "CONTRASENA_INCORRECTA");
    q.exec();

    if (ok) {
        labelErrorPassword->clear();
        stackedWidget->setCurrentIndex(1);
    } else {
        labelErrorPassword->setText("Contraseña incorrecta. Intenta de nuevo.");
        lineEditPassword->clear();
    }
}