# README - BitTorrent

**Trimiterea și recepționarea segmentelor de fișiere** între participanți (clienți și un tracker central).  
**Gestionarea fișierelor deținute** de către fiecare participant.  
**Descărcarea fișierelor dorite** din rețeaua distribuită.  
**Actualizarea continuă a stării rețelei** cu privire la fișiere și segmentele disponibile.

---

## Structura generală

Sistemul este compus din două tipuri de entități:

### **Tracker:**
- **Rol principal**: gestiunea centralizată a informațiilor despre fișierele disponibile și participanții care le dețin.
- Comunică cu clienții pentru a transmite informații despre segmentele disponibile și despre utilizatori (**peers**).

### **Clienți:**
- Fiecare client poate **deține fișiere sau segmente de fișiere**.
- Clienții pot **descărca segmente** de la alți clienți, pot **încărca segmente** la cererea altora și pot **raporta informații** despre fișierele proprii către tracker.

---

## Funcționalități

### **Tracker**

1. **Inițializare**:  
   Alocă structurile necesare pentru gestionarea informațiilor despre utilizatori și fișiere.
   
2. **Recepție inițială**:  
   Primește informații despre fișierele deținute de fiecare client.

3. **Procesare cereri**:  
   - **Cereri de segmente**: Tracker-ul comunică clienților de la care pot descărca segmentele dorite.  
   - **Actualizări**: Tracker-ul primește informații noi de la clienți despre segmentele descărcate.  
   - **Terminare**: Transmite un semnal tuturor clienților atunci când toate operațiunile au fost finalizate.

### **Clienți**

1. **Fișiere deținute**:  
   Fiecare client citește fișierele deținute și le transmite tracker-ului.

2. **Listă de dorințe**:  
   Fiecare client specifică fișierele pe care dorește să le descarce.

---

#### **Descărcare:**
- Descărcarea segmentelor de la alți clienți pe baza informațiilor primite de la tracker.
- Stocarea segmentelor descărcate într-un fișier local.

#### **Încărcare:**
- Răspunde cererilor altor clienți pentru segmentele pe care le deține.
- **Actualizare**: Raportează tracker-ului progresul descărcărilor.

---

## Fluxul principal de execuție

1. **Inițializare**:  
   Fiecare proces **MPI** este inițializat și identificat ca **tracker** (`rank 0`) sau **client** (`rank > 0`).

2. **Comunicare între entități**:
   - Tracker-ul central primește informații despre fișierele deținute și cereri pentru segmente.
   - Clienții comunică între ei pentru schimbul de segmente.

3. **Thread-uri paralele**:
   Fiecare client rulează **două thread-uri**:
   - Un thread pentru **descărcare**.
   - Un thread pentru **încărcare**.

4. **Finalizare**:  
   După completarea descărcărilor, clienții notifică tracker-ul, iar acesta trimite semnale de terminare.

---

## Structuri de date

### **file_info**:
Reprezintă informațiile despre un fișier (ID, număr de segmente, hash-uri ale segmentelor).

### **TrackerData**:
Stochează toate informațiile necesare pentru tracker, inclusiv starea segmentelor și clienților.

### **Peer_args**:
Parametrii transmiși thread-urilor pentru descărcare.

---

## Mesaje și protocoale

Programul utilizează mesaje pentru comunicare între procese **MPI**. Tipuri de mesaje:

- **MSG_ACK**: Confirmare.
- **MSG_REQUEST**: Cerere de segment.
- **MSG_SEGMENT**: Transmiterea unui segment.
- **MSG_UPDATE**: Actualizare despre segmente descărcate.
- **MSG_FINISH**: Finalizarea descărcării unui fișier.
- **MSG_TERMINATE**: Semnal pentru încheierea operațiunilor.
