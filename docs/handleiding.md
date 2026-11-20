# CareLink-thuisscherm — handleiding

*[English version](manual.md)*

Een klein scherm bij jou thuis dat de glucosewaarde toont van iemand met een
Medtronic MiniMed 770G- of 780G-insulinepomp. Zo zien de mensen die voor hem
of haar zorgen in één oogopslag hoe het gaat, zonder telefoon.

Deze handleiding heeft drie delen:

1. [Wat het is en wat je nodig hebt](#1-wat-het-is-en-wat-je-nodig-hebt),
   voor iedereen.
2. [De toestellen klaarmaken](#2-de-toestellen-klaarmaken), voor de persoon
   met technische kennis die het voor je installeert.
3. [De set bij jou thuis](#3-de-set-bij-jou-thuis): waar alles komt, wat de
   schermen tonen, en wat je doet als er iets niet lijkt te kloppen.

## 1. Wat het is en wat je nodig hebt

### Hoe het werkt

![De pomp stuurt zijn metingen naar de telefoon van de patiënt, de telefoon naar CareLink van Medtronic, en een klein kastje bij jou thuis haalt ze daar om de vijf minuten op en geeft ze door aan het scherm](img/manual-how-it-works.nl.svg)

De pomp stuurt zijn metingen al naar de telefoon van de patiënt, en de
telefoon stuurt ze naar CareLink, de onlinedienst van Medtronic. De set voegt
daar twee dingen aan toe, bij **jou** thuis:

- **Het kastje**, een kleine computer (een Raspberry Pi) naast je
  internetmodem. Om de vijf minuten haalt het de laatste meting op bij
  CareLink.
- **Het scherm**, een klein e-paperscherm van ongeveer 3 bij 3 cm. Om de vijf
  minuten wordt het wakker, haalt het de meting via je wifi bij het kastje,
  toont ze, en piept het als de pomp een alarm geeft.

De patiënt hoeft niet thuis te zijn: het scherm werkt waar die ook is, zolang
de telefoon aanstaat, dicht bij de pomp is en internet heeft.

### Wat het niet is

- **Het kan niets veranderen.** Het toont alleen cijfers. Niets in de set kan
  insuline geven, pompinstellingen wijzigen of een pompalarm uitzetten.
- **Het is geen medisch hulpmiddel en geen alarmsysteem.** De pomp en de
  telefoon blijven degene die waarschuwen. Het scherm toont dezelfde cijfers
  in een andere kamer.
- **Het loopt altijd een paar minuten achter.** De pomp, het kastje en het
  scherm werken elk in stappen van vijf minuten.
- **Het hangt af van de telefoon van de patiënt.** Staat die uit, is hij te
  ver van de pomp of zonder internet, dan meldt het scherm dat; dat is geen
  fout van de set.

### Wat je nodig hebt

| Wat | |
|---|---|
| **De twee toestellen** | het kastje en het scherm, klaargemaakt door je helper (deel 2) |
| **Internet thuis** | een modem met **één vrije netwerkaansluiting** voor het kastje. Geen vrije aansluiting: dan kan het kastje via je wifi |
| **De naam en het wachtwoord van je wifi** | het scherm maakt verbinding met je wifi. Het heeft wifi op 2,4 GHz nodig; bijna elke modem biedt dat, vaak onder dezelfde naam |
| **Een apart CareLink-account voor het scherm** | zie hieronder |
| **Twee stopcontacten** | één bij de modem voor het kastje, één waar het scherm komt |
| **Een helper met technische kennis** | om de toestellen klaar te maken en samen met jou te installeren, ongeveer 45 minuten bij je thuis |

### Een apart CareLink-account, aanvaard door de patiënt

Het kastje meldt zich bij CareLink aan met een eigen account: een
**zorgpartner-account**. Dat is een gewoon CareLink-account, alleen voor dit
scherm aangemaakt, dat de **patiënt als zorgpartner uitnodigt** vanuit zijn
of haar eigen CareLink-account, en dat de uitnodiging aanvaardt. Pas dan mag
het kastje de gegevens van de patiënt lezen.

Waarom een apart account:

- **De patiënt houdt de controle.** De zorgpartner verwijderen in het eigen
  account zet het scherm uit, zonder het eigen wachtwoord te wijzigen.
- **Het is veiliger.** Het kastje bewaart alleen de aanmelding van dit
  account, en dat kan de gegevens van één patiënt lezen, niets meer.
- **Het mag nergens anders gebruikt worden.** Met een zorgpartner-account
  kan je maar op één plaats tegelijk aangemeld zijn. Wie zich ermee aanmeldt
  op een telefoon of in een tweede kastje, legt het scherm een paar uur later
  stil.

Wie dat account beheert, houdt het **wachtwoord en het toestel dat de
tweestapscode ontvangt** (de extra code die CareLink bij het aanmelden vraagt)
bij de hand. Die zijn één keer nodig bij de installatie, en daarna ongeveer
twee keer per jaar, wanneer CareLink een nieuwe aanmelding vraagt (zie
[deel 3](#ongeveer-twee-keer-per-jaar-opnieuw-aanmelden)).

### Wie doet wat

| Wie | Wat |
|---|---|
| **De patiënt** | nodigt het zorgpartner-account uit vanuit het eigen CareLink-account |
| **Jij, het gezin** | zorgt voor de naam en het wachtwoord van de wifi, een plek voor beide toestellen, en de aanmelding met het zorgpartner-account bij de installatie en ongeveer twee keer per jaar |
| **Je helper** | maakt beide toestellen klaar, installeert ze samen met jou, en is degene die je belt als er iets mis is |

## 2. De toestellen klaarmaken

*Voor de helper.* Dit deel zegt wat erbij komt kijken en welke kennis het
vraagt. De stap-voor-stapinstructies staan (in het Engels) in het
[staging and support runbook](runbook.md), de [home installation
checklist](home-installation-checklist.md) en [what you need on your own
computer](staging-setup.md).

### Welke kennis het vraagt

Je hoeft geen programmeur te zijn. Je moet wel vlot omgaan met:

- **Instructies tot op de letter volgen**, ook commando's in een terminal
  precies zo intypen als ze er staan, en lezen wat er terugkomt.
- **Een Raspberry Pi klaarmaken**: een geheugenkaart beschrijven met
  Raspberry Pi Imager, en vanaf je laptop op de Pi inloggen via SSH, met een
  sleutel in plaats van een wachtwoord.
- **Thuisnetwerken**: een vrije aansluiting op een modem vinden, wifi op
  2,4 GHz van 5 GHz onderscheiden, en een telefoon of laptop met een bepaalde
  wifi verbinden.
- **Wachtwoorden en accounts**: per gezin een paar wachtwoorden veilig
  bewaren, en iemand door een aanmelding met tweestapscode helpen zonder ooit
  het wachtwoord te zien.
- **Het aanspreekpunt zijn**: het gezin belt jou als het scherm er vreemd
  uitziet. Het meeste kan je op afstand nakijken, via Raspberry Pi Connect.

### Boodschappenlijst

| Wat | Opmerking |
|---|---|
| M5Stack Core Ink | het scherm |
| Raspberry Pi 4 | het kastje; het model waarop dit getest is |
| microSD-kaart, 16 GB of meer | voor de Pi |
| Raspberry Pi-stroomadapter | de officiële: een zwakke adapter geeft fouten die op softwareproblemen lijken |
| Netwerkkabel | van de modem naar de Pi |
| USB-C-lader voor het scherm | een goede, niet de USB-poort van een computer |

### Het scherm: in de browser flashen

De software van het scherm staat er in ongeveer een minuut op, vanaf een
webpagina:
**[Flash the CareLink home display](https://fredvangoolen.github.io/carelink-home-display/)**.
Open ze in Chrome of Edge op een laptop, sluit de Core Ink aan met een
USB-C-kabel en druk op *Install*. Kies *Erase* voor een nieuw scherm of een
scherm dat naar een ander gezin gaat; kies niet wissen om een scherm in
gebruik bij te werken.

De wifi van het scherm en het adres van het kastje vul je later in, bij het
gezin thuis: een gewist scherm toont een QR-code die een telefoon naar de
instelpagina leidt.

### Het kastje: op je eigen bureau klaarmaken

Op je eigen bureau, vóór het bezoek, de eerste keer ongeveer een uur:

1. Zet Raspberry Pi OS Lite op de geheugenkaart met Raspberry Pi Imager, met
   een naam voor het kastje (`carelink-<naam>`) en je SSH-sleutel.
2. Log er vanaf je laptop op in en start één script, dat alles installeert:
   `appliance/provision.sh`.
3. Kies het wachtwoord voor het aanmeldscherm (het VNC-wachtwoord), druk het
   label voor de instel-hotspot af, en koppel het kastje aan Raspberry Pi
   Connect voor hulp op afstand.

Het kastje meldt zich **niet** op je bureau aan bij CareLink. Dat gebeurt bij
het gezin thuis, met het zorgpartner-account, zodra het kastje op het
thuisnetwerk zit.

### Het bezoek

Ongeveer 45 minuten, volgens de [home installation
checklist](home-installation-checklist.md): het kastje aansluiten, de
accountbeheerder op een laptop laten aanmelden bij CareLink, het scherm
instellen met een telefoon, er een goede plek voor zoeken, en nagaan dat er
een actuele meting binnenkomt.

## 3. De set bij jou thuis

![Het kastje naast de modem, in een vrije netwerkaansluiting en een stopcontact; het scherm in de keuken op zijn eigen lader, verbonden via de wifi thuis](img/manual-home-setup.nl.svg)

- **Het kastje** staat naast je internetmodem, met een netwerkkabel in een
  vrije aansluiting van de modem, en met zijn eigen stroomadapter. Het heeft
  geen scherm en vraagt niets van jou.
- **Het scherm** komt op een plek waar vaak iemand langskomt, zoals de keuken
  of de gang, op zijn eigen USB-lader. Het moet de wifi goed kunnen ontvangen:
  het scherm *Device & Network* zegt hoe goed (*Signal*: *good* of *fair* is
  in orde).

### De schermen

![De bediening van het scherm en de vijf schermen: Glucose, Trend, Statistiek, Pomp, en Device & Network](img/display-controls.nl.svg)

De **zijschakelaar** is de enige knop die je nodig hebt: duw hem omlaag voor
het volgende scherm, omhoog voor het vorige. Het scherm zelf toont Engelse
woorden; hieronder staat wat ze betekenen.

| Scherm | Toont |
|---|---|
| **Glucose** | de laatste meting, de trendpijl, de actieve insuline (*Act. insulin*), en hoe lang geleden de meting is (*Updated … ago*). Alarmen van de pomp verschijnen hier, met een piep |
| **Trend** | de laatste 3 uur als lijn (*Last 3 hours*), met streepjeslijnen bij 70 en 180, een fijne stippellijn bij 100 (het streefdoel van de pomp), en de uren onderaan. Op het scherm zijn de zones onder 70 en boven 180 gestippeld; de lege band ertussen is binnen bereik. De schaal past zich aan de metingen aan; gaat een piek boven 200, dan staat rechtsboven tot hoe hoog. De stip is de laatste meting; stopt de lijn voor de rechterrand, dan kwamen er sindsdien geen metingen binnen |
| **Statistiek** | de laatste 24 uur: tijd binnen bereik (*In range*), erboven (*Above*), eronder (*Below*), en het gemiddelde (*Average*) |
| **Pomp** | het reservoir, de batterij van de pomp, en hoe lang de sensor al zit |
| **Device & Network** | de wifi die het scherm gebruikt en hoe goed het signaal is, en de batterij van het scherm zelf. Je helper kan je vragen dit scherm voor te lezen |

Bij de volgende meting, binnen vijf minuten, keert het scherm vanzelf terug
naar *Glucose*.

### Hoe het er normaal uitziet

**De tijd op het scherm is een paar minuten oud.** Dat is normaal, altijd.

Het is een e-paperscherm: het houdt zijn laatste beeld, ook zonder stroom.
Een scherm dat gestopt is, ziet er dus precies hetzelfde uit als een scherm
dat werkt. **Kijk naar de tijd.** Is die meer dan ongeveer 15 minuten oud,
dan moet er iets gebeuren.

Komt er geen nieuwe meting, dan zegt een regel onderaan het scherm *Glucose*
waarom. Die regel verschijnt pas na ongeveer 15 minuten, zodat een korte
onderbreking nooit te zien is.

| Er staat | Betekent | Wat doe je |
|---|---|---|
| *No sensor data* | de sensor, de pomp of de telefoon van de patiënt stuurt niets | niets, het lost zichzelf op. Duurt het lang, kijk dan de telefoon van de patiënt na |
| *No new reading for …* | alles antwoordt, maar de meting verandert niet | kijk de telefoon van de patiënt en de MiniMed-app na |
| *Proxy has no data* | het kastje bereikt CareLink niet | kijk na of je internet werkt; het kastje probeert het elke minuut opnieuw |
| *CareLink login needed* | CareLink vraagt een nieuwe aanmelding | bel je helper (zie hieronder) |
| *No link to proxy* | het scherm bereikt het kastje niet | kijk na of het kastje stroom en zijn kabel heeft; herstart dan beide (hieronder) |

("Proxy" is het kastje.)

### Als het vast lijkt te zitten

1. Trek de stekker uit van **beide**: het kastje en het scherm.
2. Wacht 10 seconden en steek ze weer in.
3. Wacht 5 minuten.

Nog altijd geen actuele meting? Bel je helper.

### Ongeveer twee keer per jaar: opnieuw aanmelden

Af en toe vraagt CareLink een nieuwe aanmelding, en alleen de beheerder van
het zorgpartner-account kan dat doen. Het scherm toont dan *CareLink login
needed*. Je helper belt je en stuurt je een link; die open je thuis op een
laptop of telefoon, en je meldt je bij CareLink aan met het wachtwoord van
het zorgpartner-account en de tweestapscode. Het wachtwoord typ je zelf in;
je helper vraagt er nooit naar.

Drie dingen lijken een fout maar zijn het niet:

- een rode waarschuwing dat de pagina **"niet veilig" of "without HTTPS"** is
- een **grijs, leeg scherm** gedurende een ogenblik, voor de CareLink-pagina
  verschijnt
- de pagina die **verdwijnt** zodra de aanmelding gelukt is. Dat is net het
  teken dat het gewerkt heeft.

### Op vakantie

De set kan mee: het kastje en het scherm vinden elkaar ook in een
vakantiewoning, zelfs op een wifi die toestellen normaal van elkaar
afschermt. Vraag het je helper voor je vertrekt; de details staan (in het
Engels) in [travel.md](travel.md).

### Ook op je computer

Hetzelfde scherm *Glucose* kan ook in een venster op een computer in huis
draaien: als reserve wanneer het scherm weg is, of op het bureau van de
patiënt zelf. Je helper installeert het (instructies, in het Engels, in
[desktop/README.md](../desktop/README.md)); het heeft Python nodig, dat
gratis is, en het adres van het kastje. Met *Monitor → Test sound* hoor je
de alarmtoon. Het toont dezelfde meldingen als de tabel hierboven, maar
alleen zolang de computer aan staat en niet slaapt: een slapende computer
toont niets.

Om snel even te kijken vanaf een computer of telefoon thuis is er ook een
pagina in de browser: `http://`, de naam van het kastje en `:8081/display`,
bijvoorbeeld `http://carelink-01:8081/display`. Die is bedoeld om even te
kijken en voor je helper bij het zoeken naar een fout, niet om open te
laten staan als bewaking: een browser speelt geen geluid tot je op *Enable
alarm sound* klikt, en stopt wanneer de computer slaapt of het tabblad
dicht gaat.

---

*Dit scherm herhaalt informatie van een insulinepomp. Het is geen medisch
hulpmiddel en geen alarmsysteem. Vertrouw voor waarschuwingen op de pomp en
de telefoon.*
