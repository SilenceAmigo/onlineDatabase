#include "crow.h"	 // Crow HTTP Server
#include <sqlite3.h> // SQLite Library
#include <iostream>
#include <string>
#include <regex>
#include <json/json.h>

using namespace std;

struct FileInfo
{
	int id;
	string name;
	int size;
	string type;
	int folderId;
};

// Funktion, um die Boundary der request zu entfernen
string extractBinaryData(const string &reqBody)
{
	// Suche nach der Position des Content-Type Headers
	size_t start_pos = reqBody.find("\r\n\r\n"); // Findet den ersten Leerraum nach den Headern
	if (start_pos == string::npos)
	{
		cerr << "Kein Content gefunden!" << endl;
		return "";
	}

	// Extrahiere die Binärdaten (alles nach den Headern)
	start_pos += 4; // Um von "\r\n\r\n" weiterzumachen
	string binaryData = reqBody.substr(start_pos);

	// Finde das Ende der Boundary (vermutlich durch "--boundary" erkannt)
	size_t boundary_pos = reqBody.find("\n--", start_pos); // Suche nach der Boundary
	if (boundary_pos != string::npos)
	{
		// Extrahiere nur den binären Teil bis zur Boundary
		binaryData = reqBody.substr(start_pos, boundary_pos - start_pos);
	}

	// Entferne den letzten Zeilenumbruch, falls vorhanden
	if (!binaryData.empty() && (binaryData.back() == '\n' || binaryData.back() == '\r'))
	{
		binaryData.pop_back(); // Entfernt das letzte Zeichen (Zeilenumbruch)
	}

	return binaryData; // Gibt nur die binären Daten zurück
}

string getNameFromFile(const string &reqBody)
{
	size_t pos = reqBody.find("filename=\"");

	// Wenn "filename=\"" gefunden wurde, extrahiere den Dateinamen
	if (pos != string::npos)
	{
		// Verschiebe die Position nach dem "filename=\""
		pos += 10; // Länge von "filename=\""

		// Suche nach dem abschließenden "
		size_t end_pos = reqBody.find("\"", pos);

		// Wenn das End-Zeichen gefunden wird, extrahiere den Dateinamen
		if (end_pos != string::npos)
		{
			string filename = reqBody.substr(pos, end_pos - pos);
			return filename;
		}
	}
	return "";
}

void deleteFileFromDatabaseById(int id)
{
	sqlite3 *db;
	sqlite3_open("file_storage.db", &db); // SQLite-Datenbank öffnen

	// Zuerst den Namen und den Typ der Datei abrufen
	const char *sqlSelect = "SELECT name, type FROM files WHERE id = ?";
	sqlite3_stmt *stmt;
	int rc = sqlite3_prepare_v2(db, sqlSelect, -1, &stmt, 0);
	if (rc != SQLITE_OK)
	{
		cerr << "Fehler beim Vorbereiten des SELECT-Statements: " << sqlite3_errmsg(db) << endl;
		sqlite3_close(db);
		return;
	}

	sqlite3_bind_int(stmt, 1, id);

	string fileName;
	string fileType;
	if (sqlite3_step(stmt) == SQLITE_ROW)
	{
		const char *nameText = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 0));
		fileName = (nameText != nullptr) ? string(nameText) : "Unbenannt";

		const char *typeText = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 1));
		fileType = (typeText != nullptr) ? string(typeText) : "Unbekannt";
	}
	else
	{
		cerr << "Fehler: Datei mit ID " << id << " nicht gefunden!" << endl;
		sqlite3_finalize(stmt);
		sqlite3_close(db);
		return;
	}

	sqlite3_finalize(stmt);

	// Jetzt die Datei löschen
	const char *sqlDelete = "DELETE FROM files WHERE id = ?";
	rc = sqlite3_prepare_v2(db, sqlDelete, -1, &stmt, 0);
	if (rc != SQLITE_OK)
	{
		cerr << "Fehler beim Vorbereiten des DELETE-Statements: " << sqlite3_errmsg(db) << endl;
		sqlite3_close(db);
		return;
	}

	sqlite3_bind_int(stmt, 1, id);

	rc = sqlite3_step(stmt);
	if (rc != SQLITE_DONE)
	{
		cerr << "Fehler beim Ausführen des DELETE-Statements: " << sqlite3_errmsg(db) << endl;
	}
	else
	{
		cout << fileType << " \"" << fileName << "\" (ID: " << id << ") erfolgreich gelöscht!" << endl;
	}

	sqlite3_finalize(stmt);
	sqlite3_close(db); // SQLite-Datenbank schließen
}

void createTableIfNotExists(sqlite3 *db)
{
	// SQL-Anweisung zum Erstellen der Tabelle
	const char *sqlCreateTable =
		"CREATE TABLE IF NOT EXISTS files ("
		"id INTEGER PRIMARY KEY AUTOINCREMENT, " // Primärschlüssel
		"name TEXT NOT NULL, "					 // Dateiname
		"content BLOB, "						 // Dateiinhalt
		"size INTEGER NOT NULL, "				 // Dateigröße
		"type TEXT NOT NULL, "					 // Type
		"folderid INTEGER);";					 // foldername ist jetzt nullable

	char *errMsg;
	int rc = sqlite3_exec(db, sqlCreateTable, 0, 0, &errMsg);

	if (rc != SQLITE_OK)
	{
		cerr << "Fehler beim Erstellen der Tabelle: " << errMsg << endl;
		sqlite3_free(errMsg);
	}
}

vector<FileInfo> getFilesFromDatabase(int folderId)
{
	sqlite3 *db;
	sqlite3_open("file_storage.db", &db);

	const char *sqlSelect = "SELECT id, name, size, type, folderid FROM files WHERE folderid = ?";
	sqlite3_stmt *stmt;
	vector<FileInfo> files;

	int rc = sqlite3_prepare_v2(db, sqlSelect, -1, &stmt, 0);
	if (rc != SQLITE_OK)
	{
		cerr << "Fehler beim Vorbereiten der SELECT-Anfrage: " << sqlite3_errmsg(db) << endl;
		sqlite3_close(db);
		return files;
	}

	rc = sqlite3_bind_int(stmt, 1, folderId);
	if (rc != SQLITE_OK)
	{
		cerr << "Fehler beim Binden des folderId-Parameters: " << sqlite3_errmsg(db) << endl;
		sqlite3_finalize(stmt);
		sqlite3_close(db);
		return files;
	}

	while ((rc = sqlite3_step(stmt)) == SQLITE_ROW)
	{
		FileInfo file;
		file.id = sqlite3_column_int(stmt, 0);

		const char *nameText = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 1));
		file.name = (nameText != nullptr) ? string(nameText) : "Unbenannt";

		file.size = sqlite3_column_int(stmt, 2);

		const char *typeText = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 3));
		file.type = (typeText != nullptr) ? string(typeText) : "Unbekannt";

		if (file.type == "folder")
		{
			file.size = 0;
		}

		file.folderId = sqlite3_column_int(stmt, 4);

		files.push_back(file);
	}

	sqlite3_finalize(stmt);
	sqlite3_close(db);
	return files;
}

void deleteFolderFromDatabaseById(int id)
{
	sqlite3 *db;
	if (sqlite3_open("file_storage.db", &db) != SQLITE_OK)
	{
		std::cerr << "Fehler beim Öffnen der Datenbank: " << sqlite3_errmsg(db) << std::endl;
		return;
	}

	vector<FileInfo> files = getFilesFromDatabase(id);

	cout << endl;
	for (const FileInfo &file : files)
	{

		if (file.type == "folder")
		{
			cout << "Lösche alle files und folder im folder : " << file.name << endl;
			deleteFolderFromDatabaseById(file.id);
		}
		deleteFileFromDatabaseById(file.id);
	}
	cout << endl;

	deleteFileFromDatabaseById(id);
	sqlite3_close(db);
}

void insertFolderToDatabase(const std::string &folderName, int folderId)
{
	sqlite3 *db;
	int rc = sqlite3_open("file_storage.db", &db);
	if (rc != SQLITE_OK)
	{
		std::cerr << "Fehler beim Öffnen der Datenbank: " << sqlite3_errmsg(db) << std::endl;
		return;
	}

	createTableIfNotExists(db);

	// SQL-Anweisung um die folderid mit einzufügen
	const char *sqlInsert = "INSERT INTO files (name, content, size, type, folderid) VALUES (?, NULL, 0, ?, ?)";
	sqlite3_stmt *stmt;
	rc = sqlite3_prepare_v2(db, sqlInsert, -1, &stmt, 0);
	if (rc != SQLITE_OK)
	{
		std::cerr << "Fehler beim Vorbereiten des Insert-Statements: " << sqlite3_errmsg(db) << std::endl;
		sqlite3_close(db);
		return;
	}

	// Parameter 1: Ordnername
	rc = sqlite3_bind_text(stmt, 1, folderName.c_str(), -1, SQLITE_STATIC);
	if (rc != SQLITE_OK)
	{
		std::cerr << "Fehler beim Binden des Namens: " << sqlite3_errmsg(db) << std::endl;
	}

	// Parameter 2: Typ, hier fest "folder"
	const char *folderType = "folder";
	rc = sqlite3_bind_text(stmt, 2, folderType, -1, SQLITE_STATIC);
	if (rc != SQLITE_OK)
	{
		std::cerr << "Fehler beim Binden des Typs: " << sqlite3_errmsg(db) << std::endl;
	}

	// Parameter 3: Folder-ID
	rc = sqlite3_bind_int(stmt, 3, folderId);
	if (rc != SQLITE_OK)
	{
		std::cerr << "Fehler beim Binden der folderId: " << sqlite3_errmsg(db) << std::endl;
	}

	// Führe das Statement aus
	rc = sqlite3_step(stmt);
	if (rc != SQLITE_DONE)
	{
		std::cerr << "Fehler beim Ausführen des Insert-Statements: " << sqlite3_errmsg(db) << std::endl;
	}
	else
	{
		std::cout << "Ordner erfolgreich eingefügt!" << std::endl;
	}

	// Ressourcen freigeben
	sqlite3_finalize(stmt);
	sqlite3_close(db);
}

void insertFileToDatabase(string reqBody, string name, int size, int folderId)
{
	sqlite3 *db;
	int rc = sqlite3_open("file_storage.db", &db);
	if (rc != SQLITE_OK)
	{
		cerr << "Fehler beim Öffnen der Datenbank: " << sqlite3_errmsg(db) << endl;
		return;
	}

	createTableIfNotExists(db);

	const char *sqlInsert = "INSERT INTO files (name, content, size, type, folderid) VALUES (?, ?, ?, ?, ?)";
	sqlite3_stmt *stmt;
	rc = sqlite3_prepare_v2(db, sqlInsert, -1, &stmt, 0);
	if (rc != SQLITE_OK)
	{
		cerr << "Fehler beim Vorbereiten des Insert-Statements: " << sqlite3_errmsg(db) << endl;
		sqlite3_close(db);
		return;
	}

	// Binde die Parameter
	rc = sqlite3_bind_text(stmt, 1, name.c_str(), -1, SQLITE_STATIC);
	if (rc != SQLITE_OK)
	{
		cerr << "Fehler beim Binden des Namens: " << sqlite3_errmsg(db) << endl;
	}

	rc = sqlite3_bind_blob(stmt, 2, reqBody.c_str(), reqBody.size(), SQLITE_STATIC);
	if (rc != SQLITE_OK)
	{
		cerr << "Fehler beim Binden des Inhalts: " << sqlite3_errmsg(db) << endl;
	}

	rc = sqlite3_bind_int(stmt, 3, size);
	if (rc != SQLITE_OK)
	{
		cerr << "Fehler beim Binden der Größe: " << sqlite3_errmsg(db) << endl;
	}

	// Setze den 'type' auf "file"
	const char *fileType = "file";
	rc = sqlite3_bind_text(stmt, 4, fileType, -1, SQLITE_STATIC);
	if (rc != SQLITE_OK)
	{
		cerr << "Fehler beim Binden des Typs: " << sqlite3_errmsg(db) << endl;
	}

	rc = sqlite3_bind_int(stmt, 5, folderId);
	if (rc != SQLITE_OK)
	{
		cerr << "Fehler beim Binden der folderId: " << sqlite3_errmsg(db) << endl;
	}

	// Führe das Statement aus
	rc = sqlite3_step(stmt);
	if (rc != SQLITE_DONE)
	{
		cerr << "Fehler beim Ausführen des Insert-Statements: " << sqlite3_errmsg(db) << endl;
	}
	else
	{
		cout << "Datei erfolgreich eingefügt!" << endl;
	}

	// Ressourcen freigeben
	sqlite3_finalize(stmt);
	sqlite3_close(db);
}

void downloadFileById(int id, crow::response &res)
{
	sqlite3 *db;
	sqlite3_open("file_storage.db", &db); // SQLite-Datenbank öffnen

	const char *sqlSelect = "SELECT name, content FROM files WHERE id = ?"; // Dateiname und Inhalt abrufen
	sqlite3_stmt *stmt;
	int rc = sqlite3_prepare_v2(db, sqlSelect, -1, &stmt, 0);
	if (rc != SQLITE_OK)
	{
		cerr << "Fehler beim Vorbereiten der SELECT-Anfrage: " << sqlite3_errmsg(db) << endl;
		sqlite3_close(db);
		return;
	}

	// Binde die ID als Parameter
	sqlite3_bind_int(stmt, 1, id);

	rc = sqlite3_step(stmt);
	if (rc == SQLITE_ROW)
	{
		const char *name = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 0));
		const void *content = sqlite3_column_blob(stmt, 1);
		int size = sqlite3_column_bytes(stmt, 1);

		// Setze den HTTP Response Header für den Datei-Download
		res.set_header("Content-Disposition", "attachment; filename=" + string(name));
		res.set_header("Content-Type", "application/octet-stream");
		res.set_header("Content-Length", to_string(size));

		// Verwandle den BLOB-Inhalt in std::string und sende ihn als Antwort
		string fileContent(reinterpret_cast<const char *>(content), size);
		res.write(fileContent);
	}
	else
	{
		cerr << "Keine Datei mit der angegebenen ID gefunden!" << endl;
	}

	sqlite3_finalize(stmt); // Statement freigeben
	sqlite3_close(db);		// Datenbank schließen
}

int main()
{
	crow::SimpleApp app;

	// Route für POST-Anfrage, um den Text zu speichern
	CROW_ROUTE(app, "/upload-file/<int>").methods("POST"_method)([](const crow::request &req, crow::response &res, int folderId)
																 {
		std::string BinaryData;
		if (!req.body.empty())
		{
			BinaryData = extractBinaryData(req.body);
			std::string fileName = getNameFromFile(req.body);
			insertFileToDatabase(BinaryData, fileName, BinaryData.size(), folderId);
		}

		res.add_header("Access-Control-Allow-Origin", "*");
		res.add_header("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
		res.add_header("Access-Control-Allow-Headers", "Content-Type");
		res.code = 200;
		res.body = "Text erfolgreich gespeichert!";
		res.end(); });

	CROW_ROUTE(app, "/upload-folder/<int>").methods("POST"_method)([](const crow::request &req, int folderId)
																   {
																
		insertFolderToDatabase(req.body, folderId);
		crow::response res;
		res.add_header("Access-Control-Allow-Origin", "*");
		res.add_header("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
		res.add_header("Access-Control-Allow-Headers", "Content-Type");
		res.code = 200;
		res.body = "Text erfolgreich gespeichert!";
		return res; });

	CROW_ROUTE(app, "/delete-folder").methods("POST"_method)([](const crow::request &req)
															 {
															
	// Die ID aus dem Request-Body extrahieren
	string stringId = req.body;

	// Konvertiere die stringId in einen int
	int id;
	try {
		id = std::stoi(stringId);  // Konvertierung von string zu int
	}
	catch (const std::invalid_argument& e) {
		// Fehlerbehandlung, falls die Konvertierung fehlschlägt
		return crow::response(400, "Ungültige ID");  // Bad Request
	}
	catch (const std::out_of_range& e) {
		// Fehlerbehandlung für eine zu große Zahl
		return crow::response(400, "ID außerhalb des gültigen Bereichs");  // Bad Request
	}

	deleteFolderFromDatabaseById(id);
	
	// Erstelle die Antwort
	crow::response res;
	res.add_header("Access-Control-Allow-Origin", "*");  // Zulassen von Anfragen von jeder Domain
	res.add_header("Access-Control-Allow-Methods", "POST, OPTIONS");  // Erlaubte Methoden
	res.add_header("Access-Control-Allow-Headers", "Content-Type");  // Erlaubte Header
	res.code = 200;
	res.body = "Datei erfolgreich gelöscht!";
	return res; });

	CROW_ROUTE(app, "/delete-file").methods("POST"_method)([](const crow::request &req)
														   {
															
	// Die ID aus dem Request-Body extrahieren
	string stringId = req.body;

	// Konvertiere die stringId in einen int
	int id;
	try {
		id = std::stoi(stringId);  // Konvertierung von string zu int
	}
	catch (const std::invalid_argument& e) {
		// Fehlerbehandlung, falls die Konvertierung fehlschlägt
		return crow::response(400, "Ungültige ID");  // Bad Request
	}
	catch (const std::out_of_range& e) {
		// Fehlerbehandlung für eine zu große Zahl
		return crow::response(400, "ID außerhalb des gültigen Bereichs");  // Bad Request
	}

	// Lösche die Datei aus der Datenbank
	deleteFileFromDatabaseById(id);

	// Erstelle die Antwort
	crow::response res;
	res.add_header("Access-Control-Allow-Origin", "*");  // Zulassen von Anfragen von jeder Domain
	res.add_header("Access-Control-Allow-Methods", "POST, OPTIONS");  // Erlaubte Methoden
	res.add_header("Access-Control-Allow-Headers", "Content-Type");  // Erlaubte Header
	res.code = 200;
	res.body = "Datei erfolgreich gelöscht!";
	return res; });

	CROW_ROUTE(app, "/files/<int>").methods("GET"_method)([](int folderId)
														  {

	vector<FileInfo> files = getFilesFromDatabase(folderId);
	
	Json::Value jsonResponse;
	
	for (const FileInfo &file : files) {
		Json::Value jsonFile;
		jsonFile["id"] = file.id;
		jsonFile["name"] = file.name;
		jsonFile["size"] = file.size;
		jsonFile["type"] = file.type;
		jsonFile["folderId"] = file.folderId;
		
		jsonResponse.append(jsonFile);
	}

	// Rückgabe der Dateiinformationen als JSON
	crow::response res;
	res.add_header("Access-Control-Allow-Origin", "*"); // Erlaubt jede Quelle (*)
	res.add_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
	res.add_header("Access-Control-Allow-Headers", "Content-Type");
	res.code = 200;
	res.set_header("Content-Type", "application/json");
	res.body = jsonResponse.toStyledString(); // Umwandeln in formatierte JSON-String

	return res; });

	CROW_ROUTE(app, "/download/<int>")
	([](int id)
	 {	
		crow::response res;
		res.add_header("Access-Control-Allow-Origin", "*"); // Erlaubt jede Quelle (*)
		res.add_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
		res.add_header("Access-Control-Allow-Headers", "Content-Type");
		res.code = 200;
		res.set_header("Content-Type", "application/json");
		
		downloadFileById(id, res);
		return res; });

	// Server auf Port 8080 starten
	app.bindaddr("0.0.0.0").port(8080).multithreaded().run();

	return 0;
}
