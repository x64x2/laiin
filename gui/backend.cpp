#include "backend.hpp"

#include <QClipboard>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess> // Note: QProcess is not supported on VxWorks, iOS, tvOS, or watchOS.
#include <QUuid>
#include <QDateTime>
#include <QImage>
#include <QPixmap>
#include <QByteArray>
#include <QBuffer>
#include <QPainter>
#include <QImageReader>
#include <QImageWriter>

#include "../laiin_config.hpp"
#include "../core/version.hpp"
#include "../core/protocol/p2p/serializer.hpp"
#include "daemon_manager.hpp"
#include "../core/cart.hpp"
#include "../core/protocol/transport/client.hpp"
#include "../core/price/currency_converter.hpp" // laiin::Converter::is_supported_currency
#include "../core/price/currency_map.hpp"
#include "../core/crypto/sha256.hpp" // sha256
#include "../core/database/database.hpp"
#include "../core/tools/script.hpp"
#include "../core/settings.hpp"
#include "script_manager.hpp" // laiin::Script::get_table_string
#include "../core/tools/filesystem.hpp"
#include "../core/tools/logger.hpp"
#include "../core/tools/process.hpp"
#include "../core/category.hpp"
#include "../core/tools/string.hpp"
#include "../core/tools/timestamp.hpp"
#include "../core/crypto/rsa.hpp"
#include "enum_wrapper.hpp"
#include "../core/protocol/p2p/file_piece_hasher.hpp"


#include <future>
#include <thread>

namespace laiin_filesystem = laiin::filesystem;

laiin::Backend::Backend(QObject *parent) : QObject(parent) {}

laiin::Backend::~Backend() {
    #ifdef laiin_DEBUG
    std::cout << "backend deleted\n";
    #endif
}

QString laiin::Backend::urlToLocalFile(const QUrl &url) const
{
    return url.toLocalFile();
}

void laiin::Backend::copyTextToClipboard(const QString& text) {
    QClipboard * clipboard = QGuiApplication::clipboard();
    clipboard->setText(text);
    std::cout << "Copied text to clipboard\n";
}

QString laiin::Backend::imageToBase64(const QImage& image) {
    QByteArray byteArray;
    QBuffer buffer(&byteArray);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG"); // You can choose a different format if needed
    return QString::fromLatin1(byteArray.toBase64().data());
}

QImage laiin::Backend::base64ToImage(const QString& base64Data) {
    QByteArray byteArray = QByteArray::fromBase64(base64Data.toLatin1());
    QImageReader reader(byteArray);
    reader.setAutoTransform(true);
    const QImage image = reader.read();
    return image;
}

bool laiin::Backend::isSupportedImageDimension(int width, int height) {
    // App will crash if images are the following dimensions or larger: 1700×1700, 1800×1800, 1920×1440, 1920x1920, 2048x2048
    // Image dimensions that have been tested and won't crash the app: 1200x1200, 1920x1080, 1920x1200, 1200×1920, 1920×1280, 1280×1920, 1280×1280, 1440×1440, 1500×1500, 1600×1600
    int maxWidth = 1920;
    int maxHeight = 1280;
    int maxDimensionsRange = 1600; // Support images with dimensions up to the ranges [1600, 1600] since they won't crash the app

    // If the image size does not exceed the dimensions range
    if ((width <= maxWidth && height <= maxHeight) ||
        (height <= maxWidth && width <= maxHeight) ||
        (width <= maxDimensionsRange && height <= maxDimensionsRange)) {
        return true;
    }
    return false;
}

bool laiin::Backend::isSupportedImageSizeBytes(int sizeBytes) {
    return (sizeBytes <= laiin_MAX_IMAGE_SIZE);
}

double laiin::Backend::weightToKg(double amount, const QString& unit_name) const {
    return laiin::Converter::to_kg(amount, unit_name.toStdString());
}

double laiin::Backend::lgToKg(double amount) const {
    return laiin::Converter::lb_to_kg(amount);
}

QStringList laiin::Backend::getCurrencyList() const
{
    QStringList currency_list;
    for (const auto& [key, value] : laiin::CurrencyMap) {
        currency_list << QString::fromStdString(key);
    }
    return currency_list;
}

int laiin::Backend::getCurrencyDecimals(const QString& currency) const {
    auto map_key = currency.toUpper().toStdString();
    // Check if key exists in std::map
    if(laiin::CurrencyMap.count(map_key) > 0) {
        auto map_value = laiin::CurrencyMap[map_key];
        int decimal_places = std::get<2>(map_value);
        return decimal_places;
    }
    return 2;
}

QString laiin::Backend::getCurrencySign(const QString& currency) const {
    return QString::fromStdString(laiin::Converter::get_currency_sign(currency.toStdString()));
}

bool laiin::Backend::isSupportedCurrency(const QString& currency) const {
    return laiin::Converter::is_supported_currency(currency.toStdString());
}


QString laiin::Backend::getDurationFromNow(const QString& timestamp) const {
    return QString::fromStdString(laiin::timestamp::get_duration_from_now(timestamp.toStdString()));
}

void laiin::Backend::initializeDatabase() {
    db::Sqlite3 * database = laiin::get_user_database();
    database->execute("BEGIN;");
    
    // favorites (wishlists)
    if(!database->table_exists("favorites")) {
        database->execute("CREATE TABLE favorites("
        "user_id TEXT, "
        "listing_key TEXT, "
        "UNIQUE(user_id, listing_key)"
        ");");
    }    
    
    // cart
    if(!database->table_exists("cart")) {
        database->execute("CREATE TABLE cart(uuid TEXT NOT NULL PRIMARY KEY, "
        "user_id TEXT"
        ");");
        // cart_items
        database->execute("CREATE TABLE cart_item(id INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT, "
        "cart_id TEXT REFERENCES cart(uuid) ON DELETE CASCADE"
        ");");
        database->execute("ALTER TABLE cart_item ADD COLUMN listing_key TEXT;");
        database->execute("ALTER TABLE cart_item ADD COLUMN quantity INTEGER;");
        database->execute("ALTER TABLE cart_item ADD COLUMN seller_id TEXT;"); // for a multi-vendor cart, specifying the seller_id is important!database->execute("ALTER TABLE cart_item ADD COLUMN item_weight REAL;");
        database->execute("CREATE UNIQUE INDEX index_cart_item ON cart_item (cart_id, listing_key);"); // cart_id and listing_key duo MUST be unique for each row
    }
    
    
    database->execute("COMMIT;");
}

std::string laiin::Backend::getDatabaseHash() {
    // Get contents from data.sqlite3 file
    std::ifstream rfile (std::string("data.sqlite3").c_str(), std::ios::binary);
    std::stringstream db_content;
    db_content << rfile.rdbuf(); // dump file contents
    rfile.close();
    // Get SHA256sum of data.sqlite3 contents
    std::string sha256sum = laiin::crypto::sha256(db_content.str());
    std::cout << "sha256sum (data.sqlite3): " << sha256sum << std::endl;
    return sha256sum; // database may have to be closed first in order to get the accurate hash
}


QVariantList laiin::Backend::getCategoryList(bool sort_alphabetically) const {
    QVariantList category_list;
    
    std::vector<Category> categories = predefined_categories; // Make a copy
    
    if (sort_alphabetically) {
        std::sort(categories.begin(), categories.end(), [](const Category& a, const Category& b) {
            return a.name < b.name;
        });
    }
    
    for (const auto& category : categories) {
        QVariantMap category_object;
        category_object.insert("id", category.id);
        category_object.insert("name", QString::fromStdString(category.name));
        category_object.insert("description", QString::fromStdString(category.description));
        category_object.insert("thumbnail", QString::fromStdString(category.thumbnail));
        category_list.append(category_object);
    }

    return category_list;
}


QVariantList laiin::Backend::getSubCategoryList(int category_id, bool sort_alphabetically) const {
    QVariantList subcategory_list;
    
    std::vector<Subcategory> subcategories = get_subcategories_by_category_id(category_id);
    
    if (sort_alphabetically) {
        std::sort(subcategories.begin(), subcategories.end(), [](const Subcategory& a, const Subcategory& b) {
            return a.name < b.name;
        });
    }
    
    for (const Subcategory& subcategory : subcategories) {
        QVariantMap subcategory_obj;
        subcategory_obj.insert("id", subcategory.id);
        subcategory_obj.insert("name", QString::fromStdString(subcategory.name));
        subcategory_obj.insert("description", QString::fromStdString(subcategory.description));
        subcategory_obj.insert("thumbnail", QString::fromStdString(subcategory.thumbnail));
        subcategory_obj.insert("category_id", subcategory.category_id);
        
        subcategory_list.append(subcategory_obj);
    }
    
    return subcategory_list;
}

int laiin::Backend::getCategoryIdByName(const QString& category_name) const {
    return get_category_id_by_name(category_name.toStdString());
}

int laiin::Backend::getSubCategoryIdByName(const QString& subcategory_name) const {
    return get_subcategory_id_by_name(subcategory_name.toStdString());
}

int laiin::Backend::getCategoryProductCount(int category_id) const {
    db::Sqlite3 * database = laiin::get_database();

    std::string query = "SELECT COUNT(*) FROM (SELECT DISTINCT search_term, key FROM mappings WHERE search_term = ?)";
    std::string category = get_category_name_by_id(category_id);

    int category_product_count = database->get_integer_params(query, { category });
    return category_product_count;
}

bool laiin::Backend::hasSubCategory(int category_id) const {
    std::vector<Subcategory> subcategories = get_subcategories_by_category_id(category_id);
    return (!subcategories.empty());
}


bool laiin::Backend::saveAvatarImage(const QString& fileName, const QString& userAccountKey) {
    std::string config_path = laiin_DEFAULT_CONFIGURATION_PATH;
    std::string data_folder = config_path + "/" + laiin_DATA_FOLDER_NAME;
    std::string avatars_folder = data_folder + "/" + laiin_AVATAR_FOLDER_NAME;
    
    std::string image_file = fileName.toStdString(); // Full path with file name
    std::string image_name = image_file.substr(image_file.find_last_of("\\/") + 1);// get filename from path (complete base name)
    image_name = image_name.substr(0, image_name.find_last_of(".")); // remove ext
    std::string image_name_hash = laiin::crypto::sha256(image_name);
    std::string image_ext = image_file.substr(image_file.find_last_of(".") + 1);
    
    // datastore/avatars/<account_key>
    std::string key_folder = avatars_folder + "/" + userAccountKey.toStdString();
    if (!laiin_filesystem::is_directory(key_folder)) {
        if (!laiin_filesystem::make_directory(key_folder)) {
            laiin::print("Failed to create folder \"" + key_folder + "\" (ᵕ人ᵕ)!", 1);
            return false;
        }
        laiin::print("\033[1;97;49mcreated path \"" + key_folder + "\"");
    }

    // Generate the final destination path
    std::string destinationPath = key_folder + "/" + (image_name_hash + "." + image_ext);
    // Check if image already exists in cache so that we do not export the same image more than once
    if(!laiin_filesystem::is_file(destinationPath)) {
        QImage sourceImage;
        sourceImage.load(fileName);
        QSize imageSize = sourceImage.size();
        
        if(isSupportedImageDimension(imageSize.width(), imageSize.height())) {
            QFile sourceFile(fileName);
            if(sourceFile.copy(QString::fromStdString(destinationPath))) {
                laiin::print("copied \"" + fileName.toStdString() + "\" to \"" + key_folder + "\"", 3);
                sourceFile.close();
                return true;
            }
            sourceFile.close();
        }
    }
    
    return false;
}

bool laiin::Backend::saveProductThumbnail(const QString& fileName, const QString& listingKey) {
    std::string config_path = laiin_DEFAULT_CONFIGURATION_PATH;
    std::string data_folder = config_path + "/" + laiin_DATA_FOLDER_NAME;
    std::string listings_folder = data_folder + "/" + laiin_CATALOG_FOLDER_NAME;

    // datastore/listings/<listing_key>
    std::string key_folder = listings_folder + "/" + listingKey.toStdString();
    if (!laiin_filesystem::is_directory(key_folder)) {
        if (!laiin_filesystem::make_directory(key_folder)) {
            laiin::print("Failed to create folder \"" + key_folder + "\" (ᵕ人ᵕ)!", 1);
            return false;
        }
        laiin::print("\033[1;97;49mcreated path \"" + key_folder + "\"");
    }

    // Generate the final destination path
    std::string thumbnail_image = "thumbnail.jpg";
    std::string destinationPath = key_folder + "/" + thumbnail_image;
    // Check if image already exists in cache so that we do not export the same image more than once
    if(!laiin_filesystem::is_file(destinationPath)) {
        // Hopefully the image does not exceed 32 kB in file size :S
        QImage sourceImage;
        sourceImage.load(fileName);
        QSize imageSize = sourceImage.size();
        int maxWidth = 192; // Set the maximum width for the resized image
        int maxHeight = 192; // Set the maximum height for the resized image
        
        // Convert the transparent background to white if necessary
        if (sourceImage.hasAlphaChannel()) {
            QImage convertedImage = QImage(sourceImage.size(), QImage::Format_RGB32);
            convertedImage.fill(Qt::white);
            QPainter painter(&convertedImage);
            painter.drawImage(0, 0, sourceImage);
            painter.end();
            sourceImage = convertedImage;
        }

        // Check if the image size is smaller than the maximum size
        if (imageSize.width() <= maxWidth && imageSize.height() <= maxHeight) {
            // Keep the original image since it's already within the size limits
        } else {
            // Calculate the new size while maintaining the aspect ratio
            QSize newSize = imageSize.scaled(maxWidth, maxHeight, Qt::KeepAspectRatio);

            // Resize the image if it exceeds the maximum dimensions
            if (imageSize != newSize) {
                sourceImage = sourceImage.scaled(newSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
            }
        }

        // Convert the QImage to QPixmap for further processing or saving
        QPixmap resizedPixmap = QPixmap::fromImage(sourceImage);

        // Save the resized image
        resizedPixmap.save(QString::fromStdString(destinationPath), "JPEG");
    }
    
    laiin::print("exported \"" + thumbnail_image + "\" to \"" + key_folder + "\"", 3);
    return true;
}

bool laiin::Backend::saveProductImage(const QString& fileName, const QString& listingKey) {
    std::string config_path = laiin_DEFAULT_CONFIGURATION_PATH;
    std::string data_folder = config_path + "/" + laiin_DATA_FOLDER_NAME;
    std::string listings_folder = data_folder + "/" + laiin_CATALOG_FOLDER_NAME;
    
    std::string image_file = fileName.toStdString(); // Full path with file name
    std::string image_name = image_file.substr(image_file.find_last_of("\\/") + 1);// get filename from path (complete base name)
    image_name = image_name.substr(0, image_name.find_last_of(".")); // remove ext
    std::string image_name_hash = laiin::crypto::sha256(image_name);
    std::string image_ext = image_file.substr(image_file.find_last_of(".") + 1);
        
    // datastore/listings/<listing_key>
    std::string key_folder = listings_folder + "/" + listingKey.toStdString();
    if (!laiin_filesystem::is_directory(key_folder)) {
        if (!laiin_filesystem::make_directory(key_folder)) {
            laiin::print("Failed to create folder \"" + key_folder + "\" (ᵕ人ᵕ)!", 1);
            return false;
        }
        laiin::print("\033[1;97;49mcreated path \"" + key_folder + "\"");
    }

    // Generate the final destination path
    std::string destinationPath = key_folder + "/" + (image_name_hash + "." + image_ext);
    // Check if image already exists in cache so that we do not export the same image more than once
    if(!laiin_filesystem::is_file(destinationPath)) {
        QImage sourceImage;
        sourceImage.load(fileName);
        QSize imageSize = sourceImage.size();
        
        if(isSupportedImageDimension(imageSize.width(), imageSize.height())) {
            QFile sourceFile(fileName);
            if(sourceFile.copy(QString::fromStdString(destinationPath))) {
                laiin::print("copied \"" + fileName.toStdString() + "\" to \"" + key_folder + "\"", 3);
                sourceFile.close();
                return true;
            }
            sourceFile.close();
        }
    }
    
    return false;
}

QVariantMap laiin::Backend::uploadImageToObject(const QString& fileName, int imageId) {
    QVariantMap image;
    
    // Determine piece length
    QImage imageFile(fileName);
    QFileInfo fileInfo(fileName);
    qint64 size = fileInfo.size();
    QSize dimensions = imageFile.size();
    
    size_t piece_size = 16384; // each file piece will be 16 KB
    const size_t MEGABYTE = (laiin_MAX_IMAGE_SIZE / 2);
    if(size >= laiin_MAX_IMAGE_SIZE) { piece_size = MEGABYTE; } // If image is 2 MB or more, piece length = 1 MB
    else if(size >= MEGABYTE) { piece_size = 524288; } // If image is up to 1 MB, piece_size = 512 KB
    else if(size >= 524288) { piece_size = 262144; } // If image is up to 512 KB, piece_size = 256 KB
    else if(size >= 262144) { piece_size = 131072; } // If image is up to 256 KB, piece_size = 128 KB
    else if(size >= 131072) { piece_size = 65536; } // If image is is up to 128 KB, piece_size = 64 KB
    else if(size >= 65536) { piece_size = 32768; } // If image is is up to 64 KB, piece_size = 32 KB
    else if(size >= 32768) { piece_size = 16384; } // If image is is up to 32 KB, piece_size = 16 KB
    
    // Hash image file pieces
    laiin::FilePieceHasher hasher(piece_size);
    std::vector<laiin::FilePiece> file_pieces = hasher.hash_file(fileName.toStdString());
    if(file_pieces.empty()) {
        std::cerr << "Product upload image is either empty or failed to load\n";
        return {};
    }
    
    size_t file_size = 0;
    QStringList piecesList;
    QByteArray imageData;
    for (size_t i = 0; i < file_pieces.size(); ++i) {
        file_size += file_pieces[i].bytes;
        piecesList.append(QString::fromStdString(file_pieces[i].hash));
        imageData.append(reinterpret_cast<const char*>(file_pieces[i].data.data()), static_cast<int>(file_pieces[i].data.size()));
    }
    
    // Create the image VariantMap (object)
    assert(file_size == size);
    std::string image_file = fileName.toStdString(); // Full path with file name
    std::string image_name = image_file.substr(image_file.find_last_of("\\/") + 1);
    image_name = image_name.substr(0, image_name.find_last_of(".")); // Remove ext
    std::string image_name_hash = laiin::crypto::sha256(image_name);
    std::string image_ext = image_file.substr(image_file.find_last_of(".") + 1);
    image["name"] = QString::fromStdString(image_name_hash + "." + image_ext);//fileName;
    image["size"] = QVariant::fromValue(static_cast<qint64>(file_size));
    image["id"] = imageId;
    image["source"] = fileName;
    image["piece_size"] = QVariant::fromValue(static_cast<qint64>(piece_size));
    image["pieces"] = piecesList;
    image["data"] = imageData;
    // Extra parameters - will only be used for checking dimensions
    image["width"] = dimensions.width();
    image["height"] = dimensions.height();
    
    return image;
}

int laiin::Backend::getProductStarCount(const QVariantList& product_ratings) {
    // Get total number of star ratings for a specific product
    return product_ratings.size();
}

int laiin::Backend::getProductStarCount(const QString& product_id) {    
    QVariantList product_ratings = getProductRatings(product_id);
    return getProductStarCount(product_ratings);
}

int laiin::Backend::getProductStarCount(const QVariantList& product_ratings, int star_number) {
    // Get total number of N star ratings for a specific product
    if(star_number > 5) star_number = 5;
    if(star_number < 1) star_number = 1;
    int star_count = 0;
    for (const QVariant& variant : product_ratings) {
        QVariantMap rating_obj = variant.toMap();
        int rating_stars = rating_obj["stars"].toInt();
        if (rating_stars == star_number) {
            star_count++;
        }
    }
    return star_count;
}

int laiin::Backend::getProductStarCount(const QString& product_id, int star_number) {
    QVariantList product_ratings = getProductRatings(product_id);
    return getProductStarCount(product_id, star_number);
}

float laiin::Backend::getProductAverageStars(const QVariantList& product_ratings) {
    // Get number of star ratings for a specific product
    int total_star_ratings = product_ratings.size();
    if(total_star_ratings == 0) return 0.0f;
    // Get number of 1, 2, 3, 4, and 5 star_ratings
    int one_star_count = 0, two_star_count = 0, three_star_count = 0, four_star_count = 0, five_star_count = 0;
    for (const QVariant& variant : product_ratings) {
        QVariantMap rating_obj = variant.toMap();
        int rating_stars = rating_obj["stars"].toInt();
        if (rating_stars == 1) {
            one_star_count++;
        } else if (rating_stars == 2) {
            two_star_count++;
        } else if (rating_stars == 3) {
            three_star_count++;
        } else if (rating_stars == 4) {
            four_star_count++;
        } else if (rating_stars == 5) {
            five_star_count++;
        }
    }
    // Now calculate the average stars 
    float average_stars = (
        (1 * static_cast<float>(one_star_count)) + 
        (2 * static_cast<float>(two_star_count)) + 
        (3 * static_cast<float>(three_star_count)) + 
        (4 * static_cast<float>(four_star_count)) + 
        (5 * static_cast<float>(five_star_count))) / total_star_ratings;
    return average_stars;
}

float laiin::Backend::getProductAverageStars(const QString& product_id) {
    QVariantList product_ratings = getProductRatings(product_id);
    return getProductAverageStars(product_ratings);
}

QVariantList laiin::Backend::getProductRatings(const QString& product_id) {
    Client * client = Client::get_main_client();
    
    std::string command = "SELECT DISTINCT key FROM mappings WHERE search_term = $1 AND content = 'product_rating'";
    db::Sqlite3 * database = laiin::get_database();
    sqlite3_stmt * stmt = nullptr;
    // Prepare (compile) statement
    if(sqlite3_prepare_v2(database->get_handle(), command.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        laiin::print("sqlite3_prepare_v2: " + std::string(sqlite3_errmsg(database->get_handle())), 1);
        return {};
    }
    // Bind value to parameter arguments
    QByteArray productIdByteArray = product_id.toUtf8();
    if(sqlite3_bind_text(stmt, 1, productIdByteArray.constData(), productIdByteArray.length(), SQLITE_STATIC) != SQLITE_OK) {
        laiin::print("sqlite3_bind_text: " + std::string(sqlite3_errmsg(database->get_handle())), 1);
        sqlite3_finalize(stmt);
        return {};
    }
    
    QVariantList product_ratings;
    // Get all table values row by row
    while(sqlite3_step(stmt) == SQLITE_ROW) {
        for(int i = 0; i < sqlite3_column_count(stmt); i++) { 
            QVariantMap product_rating_obj; // Create object for each key (row)
            
            std::string column_value = (sqlite3_column_text(stmt, i) == nullptr) ? "NULL" : reinterpret_cast<const char *>(sqlite3_column_text(stmt, i));//std::cout << column_value  << " (" << i << ")" << std::endl;
            if(column_value == "NULL") continue; // Skip invalid columns
            QString key = QString::fromStdString(column_value);//std::cout << key.toStdString() << "\n";
            
            // Get the value of the corresponding key from the DHT
            std::string response;
            client->get(key.toStdString(), response); // TODO: error handling
            std::cout << "Received response (get): " << response << "\n";
            // Parse the response
            nlohmann::json json = nlohmann::json::parse(response);
            if(json.contains("error")) {
                std::string response2;
                client->remove(key.toStdString(), response2);
                std::cout << "Received response (remove): " << response2 << "\n";
                //emit productRatingsChanged();
                continue; // Key is lost or missing from DHT, skip to next iteration
            }
            
            const auto& response_obj = json["response"];
            assert(response_obj.is_object());
            if (response_obj.contains("value") && response_obj["value"].is_string()) {
                const auto& value = response_obj["value"].get<std::string>();
                nlohmann::json value_obj = nlohmann::json::parse(value);
                assert(value_obj.is_object());//std::cout << value_obj.dump(4) << "\n";
                std::string metadata = value_obj["metadata"].get<std::string>();
                if (metadata != "product_rating") { std::cerr << "Invalid metadata. \"product_rating\" expected, got \"" << metadata << "\" instead\n"; continue; }
                product_rating_obj.insert("key", key);
                product_rating_obj.insert("rater_id", QString::fromStdString(value_obj["rater_id"].get<std::string>()));
                product_rating_obj.insert("comments", QString::fromStdString(value_obj["comments"].get<std::string>()));
                product_rating_obj.insert("signature", QString::fromStdString(value_obj["signature"].get<std::string>()));
                product_rating_obj.insert("stars", value_obj["stars"].get<int>());
                if(value_obj.contains("expiration_date") && value_obj["expiration_date"].is_string()) {
                    product_rating_obj.insert("expiration_date", QString::fromStdString(value_obj["expiration_date"].get<std::string>()));
                }
            }
            
            product_ratings.append(product_rating_obj);
        }
    }
    
    sqlite3_finalize(stmt);
    
    return product_ratings;
}

int laiin::Backend::getSellerGoodRatings(const QVariantList& seller_ratings) {
    int good_ratings_count = 0;
    // Get seller's good (positive) ratings
    for (const QVariant& variant : seller_ratings) {
        QVariantMap rating_obj = variant.toMap();
        int rating_score = rating_obj["score"].toInt();
        if(rating_score == 1) {
            good_ratings_count++;
        }
    }
    return good_ratings_count;
}

int laiin::Backend::getSellerGoodRatings(const QString& user_id) {
    QVariantList seller_ratings = getSellerRatings(user_id);
    return getSellerGoodRatings(seller_ratings);
}

int laiin::Backend::getSellerBadRatings(const QVariantList& seller_ratings) {
    int bad_ratings_count = 0;
    // Get seller's bad (negative) ratings
    for (const QVariant& variant : seller_ratings) {
        QVariantMap rating_obj = variant.toMap();
        int rating_score = rating_obj["score"].toInt();
        if(rating_score == 0) {
            bad_ratings_count++;
        }
    }    
    return bad_ratings_count;
}

int laiin::Backend::getSellerBadRatings(const QString& user_id) {
    QVariantList seller_ratings = getSellerRatings(user_id);
    return getSellerBadRatings(seller_ratings);
}

int laiin::Backend::getSellerRatingsCount(const QVariantList& seller_ratings) {
    return seller_ratings.size();
}

int laiin::Backend::getSellerRatingsCount(const QString& user_id) {
    QVariantList seller_ratings = getSellerRatings(user_id);
    return getSellerRatingsCount(seller_ratings);
}

int laiin::Backend::getSellerReputation(const QVariantList& seller_ratings) {
    int good_ratings_count = 0, bad_ratings_count = 0;
    int ratings_count = seller_ratings.size();
    if(ratings_count <= 0) return 0; // seller has not yet been rated so his or her reputation will be 0%
    // Get seller's good (positive) ratings
    for (const QVariant& variant : seller_ratings) {
        QVariantMap rating_obj = variant.toMap();
        int rating_score = rating_obj["score"].toInt();
        if(rating_score == 1) {
            good_ratings_count++;
        }
    }
    // Calculate seller reputation
    double reputation = (good_ratings_count / static_cast<double>(ratings_count)) * 100;
    return static_cast<int>(reputation); // convert reputation to an integer (for easier readability)
}

int laiin::Backend::getSellerReputation(const QString& user_id) {
    QVariantList seller_ratings = getSellerRatings(user_id);
    return getSellerReputation(seller_ratings);
}

// returns an array of ratings objects
QVariantList laiin::Backend::getSellerRatings(const QString& user_id) {
    Client * client = Client::get_main_client();
    
    std::string command = "SELECT DISTINCT key FROM mappings WHERE search_term = $1 AND content = 'seller_rating'";
    db::Sqlite3 * database = laiin::get_database();
    sqlite3_stmt * stmt = nullptr;
    // Prepare (compile) statement
    if(sqlite3_prepare_v2(database->get_handle(), command.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        laiin::print("sqlite3_prepare_v2: " + std::string(sqlite3_errmsg(database->get_handle())), 1);
        return {};
    }
    // Bind value to parameter arguments
    QByteArray userIdByteArray = user_id.toUtf8();
    if(sqlite3_bind_text(stmt, 1, userIdByteArray.constData(), userIdByteArray.length(), SQLITE_STATIC) != SQLITE_OK) {
        laiin::print("sqlite3_bind_text: " + std::string(sqlite3_errmsg(database->get_handle())), 1);
        sqlite3_finalize(stmt);
        return {};
    }
    
    QVariantList seller_ratings;
    // Get all table values row by row
    while(sqlite3_step(stmt) == SQLITE_ROW) {
        for(int i = 0; i < sqlite3_column_count(stmt); i++) { 
            QVariantMap seller_rating_obj; // Create object for each key (row)
            
            std::string column_value = (sqlite3_column_text(stmt, i) == nullptr) ? "NULL" : reinterpret_cast<const char *>(sqlite3_column_text(stmt, i));//std::cout << column_value  << " (" << i << ")" << std::endl;
            if(column_value == "NULL") continue; // Skip invalid columns
            QString key = QString::fromStdString(column_value);//std::cout << key.toStdString() << "\n";
            
            // Get the value of the corresponding key from the DHT
            std::string response;
            client->get(key.toStdString(), response); // TODO: error handling
            std::cout << "Received response (get): " << response << "\n";
            // Parse the response
            nlohmann::json json = nlohmann::json::parse(response);
            if(json.contains("error")) {
                std::string response2;
                client->remove(key.toStdString(), response2);
                std::cout << "Received response (remove): " << response2 << "\n";
                //emit sellerRatingsChanged();
                continue; // Key is lost or missing from DHT, skip to next iteration
            }
            
            const auto& response_obj = json["response"];
            assert(response_obj.is_object());
            if (response_obj.contains("value") && response_obj["value"].is_string()) {
                const auto& value = response_obj["value"].get<std::string>();
                nlohmann::json value_obj = nlohmann::json::parse(value);
                assert(value_obj.is_object());//std::cout << value_obj.dump(4) << "\n";
                std::string metadata = value_obj["metadata"].get<std::string>();
                if (metadata != "seller_rating") { std::cerr << "Invalid metadata. \"seller_rating\" expected, got \"" << metadata << "\" instead\n"; continue; }
                seller_rating_obj.insert("key", key);
                seller_rating_obj.insert("rater_id", QString::fromStdString(value_obj["rater_id"].get<std::string>()));
                seller_rating_obj.insert("comments", QString::fromStdString(value_obj["comments"].get<std::string>()));
                seller_rating_obj.insert("signature", QString::fromStdString(value_obj["signature"].get<std::string>()));
                seller_rating_obj.insert("score", value_obj["score"].get<int>());
            }
            
            seller_ratings.append(seller_rating_obj);
        }
    }
    
    sqlite3_finalize(stmt);
    
    return seller_ratings;
}

QString laiin::Backend::getDisplayNameByUserId(const QString& user_id) {
    db::Sqlite3 * database = laiin::get_database();
    if(!database) throw std::runtime_error("database is NULL");
    std::string key = database->get_text_params("SELECT key FROM mappings WHERE search_term = ?1 AND content = 'user' LIMIT 1;", { user_id.toStdString() });
    if(key.empty()) return user_id; // Key will never be empty as long as it exists in DHT + database

    std::string display_name = database->get_text_params("SELECT search_term FROM mappings WHERE key = ?1 AND LENGTH(search_term) <= 30 AND content = 'user'", { key });
    if(!display_name.empty()) {
        return QString::fromStdString(display_name);
    }
    // If the display name happens to be empty then it means the user's account (DHT) key is lost or missing
    // or more often than not it is because the user did not set a display name, so deleting the key from the database for this reason is stupid, dangerous, and will have unintended consequences
    // so its best to just return the user_id
    if(display_name.empty()) {
        // do nothing
    }
    return user_id;
}

QString laiin::Backend::getKeyByUserId(const QString& user_id) { // not currently in use
    db::Sqlite3 * database = laiin::get_database();
    if(!database) throw std::runtime_error("database is NULL");
    std::string key = database->get_text_params("SELECT key FROM mappings WHERE search_term = $1 AND content = 'user' LIMIT 1;", { user_id.toStdString() });
    return QString::fromStdString(key);
}

QVariantMap laiin::Backend::getUser(const QString& user_id) {
    Client * client = Client::get_main_client();
    
    db::Sqlite3 * database = laiin::get_database();
    if(!database) throw std::runtime_error("database is NULL");
    std::string key = database->get_text_params("SELECT key FROM mappings WHERE search_term = $1 AND content = 'user' LIMIT 1;", { user_id.toStdString() });
    if(key.empty()) return {};
    // Get the value of the corresponding key from the DHT
    std::string response;
    client->get(key, response); // TODO: error handling
    std::cout << "Received response (get): " << response << "\n";
    // Parse the response
    nlohmann::json json = nlohmann::json::parse(response);
    if(json.contains("error")) {
        std::string response2;
        client->remove(key, response2);
        std::cout << "Received response (remove): " << response2 << "\n";
        return {}; // Key is lost or missing from DHT, skip to next iteration
    }
    
    QVariantMap user_object;
            
    const auto& response_obj = json["response"];
    assert(response_obj.is_object());
    if (response_obj.contains("value") && response_obj["value"].is_string()) {
        const auto& value = response_obj["value"].get<std::string>();
        nlohmann::json value_obj = nlohmann::json::parse(value);
        assert(value_obj.is_object());//std::cout << value_obj.dump(4) << "\n";
        std::string metadata = value_obj["metadata"].get<std::string>();
        if (metadata != "user") { std::cerr << "Invalid metadata. \"user\" expected, got \"" << metadata << "\" instead\n"; return {}; }
        user_object.insert("key", QString::fromStdString(key));
        if(value_obj.contains("avatar") && value_obj["avatar"].is_object()) {
            const auto& avatar_obj = value_obj["avatar"];
            QVariantMap avatar;
            avatar.insert("name", QString::fromStdString(avatar_obj["name"].get<std::string>()));
            avatar.insert("piece_size", avatar_obj["piece_size"].get<int>());
            QStringList piecesList;
            for(const auto& piece : avatar_obj["pieces"].get<std::vector<std::string>>()) {
                piecesList.append(QString::fromStdString(piece));
            }
            avatar.insert("pieces", piecesList);
            avatar.insert("size", avatar_obj["size"].get<int>());
            user_object.insert("avatar", avatar);
        }
        user_object.insert("created_at", QString::fromStdString(value_obj["created_at"].get<std::string>()));
        if(value_obj.contains("display_name") && value_obj["display_name"].is_string()) {
            user_object.insert("display_name", QString::fromStdString(value_obj["display_name"].get<std::string>()));
        }
        user_object.insert("monero_address", QString::fromStdString(value_obj["monero_address"].get<std::string>()));
        user_object.insert("public_key", QString::fromStdString(value_obj["public_key"].get<std::string>()));
        user_object.insert("signature", QString::fromStdString(value_obj["signature"].get<std::string>()));
        user_object.insert("user_id", QString::fromStdString(value_obj["monero_address"].get<std::string>())); // alias for "monero_address"
    }

    return user_object;
}

int laiin::Backend::getAccountAge(const QString& userId) {
    return laiin::User::get_account_age(userId.toStdString());
}

int laiin::Backend::getAccountAge(const QVariantMap& userMap) {
    if(!userMap.isEmpty()) {
        if(userMap.contains("created_at")) {
            std::string iso8601 = userMap.value("created_at").toString().toStdString();
            -
            std::tm t = {};
            std::istringstream ss(iso8601);
            ss >> std::get_time(&t, "%Y-%m-%dT%H:%M:%S");
    
            // Handling optional fractional seconds and timezone offset
            if (ss.fail()) {
                ss.clear();
                ss.str(iso8601);
                ss >> std::get_time(&t, "%Y-%m-%dT%H:%M:%S.%f");
            }
    
            if (ss.fail()) {
                ss.clear();
                ss.str(iso8601);
                ss >> std::get_time(&t, "%Y-%m-%dT%H:%M:%SZ");
            }

            if (ss.fail()) {
                throw std::runtime_error("Failed to parse ISO 8601 timestamp");
            }
            -
            auto account_creation_time = std::chrono::system_clock::from_time_t(std::mktime(&t));
            auto now = std::chrono::system_clock::now();

            auto duration = now - account_creation_time;

            using std_chrono_days = std::chrono::duration<int, std::ratio<86400>>;
            auto days = std::chrono::duration_cast<std_chrono_days>(duration).count();
            auto years = days / 365;
            days %= 365;
            auto months = days / 30;
            days %= 30;

            return days;
        }
    }
    return -1;
}

int laiin::Backend::getCartMaximumItems() {
    return laiin::Cart::get_max_items();
}

int laiin::Backend::getCartMaximumQuantity() {
    return laiin::Cart::get_max_quantity();
}

// not really used at the moment
int laiin::Backend::getStockAvailable(const QString& product_id) {
    Client * client = Client::get_main_client();

    db::Sqlite3 * database = laiin::get_database();
    if(!database) throw std::runtime_error("database is NULL");
    std::string key = database->get_text_params("SELECT key FROM mappings WHERE search_term = $1 AND content = 'listing'", { product_id.toStdString() });
    if(key.empty()) return 0;
    // Get the value of the corresponding key from the DHT
    std::string response;
    client->get(key, response); // TODO: error handling
    std::cout << "Received response (get): " << response << "\n";
    // Parse the response
    nlohmann::json json = nlohmann::json::parse(response);
    if(json.contains("error")) {
        std::string response2;
        client->remove(key, response2);
        std::cout << "Received response (remove): " << response2 << "\n";
        return 0; // Key is lost or missing from DHT, return 
    }    
    
    const auto& response_obj = json["response"];
    assert(response_obj.is_object());
    if (response_obj.contains("value") && response_obj["value"].is_string()) {
        const auto& value = response_obj["value"].get<std::string>();
        nlohmann::json value_obj = nlohmann::json::parse(value);
        assert(value_obj.is_object());//std::cout << value_obj.dump(4) << "\n";
        std::string metadata = value_obj["metadata"].get<std::string>();
        if (metadata != "listing") { std::cerr << "Invalid metadata. \"listing\" expected, got \"" << metadata << "\" instead\n"; return 0; }
        int quantity = value_obj["quantity"].get<int>();
        return quantity;
    }
    
    return 0;
}

QVariantList laiin::Backend::getInventory(const QString& user_id, bool hide_illicit_items) {
    Client * client = Client::get_main_client();
    
    laiin::db::Sqlite3 * database = laiin::get_database();
    if(!database) throw std::runtime_error("database is NULL");
    std::string command = "SELECT DISTINCT key FROM mappings WHERE search_term = ?1 AND content = 'listing'";
    sqlite3_stmt * stmt = nullptr;
    // Prepare (compile) statement
    if(sqlite3_prepare_v2(database->get_handle(), command.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        laiin::print("sqlite3_prepare_v2: " + std::string(sqlite3_errmsg(database->get_handle())), 1);
        return {};
    }
    // Bind user_id to TEXT
    QByteArray userIdByteArray = user_id.toUtf8();
    if(sqlite3_bind_text(stmt, 1, userIdByteArray.constData(), userIdByteArray.length(), SQLITE_STATIC) != SQLITE_OK) {
        laiin::print("sqlite3_bind_text (arg: 1): " + std::string(sqlite3_errmsg(database->get_handle())), 1);
        sqlite3_finalize(stmt);
        return {};
    }    
    // Check whether the prepared statement returns no data (for example an UPDATE)
    if(sqlite3_column_count(stmt) == 0) {
        laiin::print("No data found. Be sure to use an appropriate SELECT statement", 1);
        return {};
    }
    
    QVariantList inventory_array;
    // Get all table values row by row
    while(sqlite3_step(stmt) == SQLITE_ROW) {
        QVariantMap inventory_object; // Create an object for each row
        QVariantList product_images;
        QStringList product_categories;

        for(int i = 0; i < sqlite3_column_count(stmt); i++) {
            std::string column_value = (sqlite3_column_text(stmt, i) == nullptr) ? "NULL" : reinterpret_cast<const char *>(sqlite3_column_text(stmt, i));//std::cout << column_value  << " (" << i << ")" << std::endl;
            if(column_value == "NULL") continue; // Skip invalid columns
            QString key = QString::fromStdString(column_value);
            // Get the value of the corresponding key from the DHT
            std::string response;
            client->get(key.toStdString(), response); // TODO: error handling
            std::cout << "Received response (get): " << response << "\n";
            // Parse the response
            nlohmann::json json = nlohmann::json::parse(response);
            if(json.contains("error")) {
                std::string response2;
                client->remove(key.toStdString(), response2);
                std::cout << "Received response (remove): " << response2 << "\n";
                continue; // Key is lost or missing from DHT, skip to next iteration
            }
            
            const auto& response_obj = json["response"];
            assert(response_obj.is_object());
            if (response_obj.contains("value") && response_obj["value"].is_string()) {
                const auto& value = response_obj["value"].get<std::string>();
                nlohmann::json value_obj = nlohmann::json::parse(value);
                assert(value_obj.is_object());//std::cout << value_obj.dump(4) << "\n";
                std::string metadata = value_obj["metadata"].get<std::string>();
                if (metadata != "listing") { std::cerr << "Invalid metadata. \"listing\" expected, got \"" << metadata << "\" instead\n"; continue; }
                inventory_object.insert("key", key);
                inventory_object.insert("listing_uuid", QString::fromStdString(value_obj["id"].get<std::string>()));
                inventory_object.insert("seller_id", QString::fromStdString(value_obj["seller_id"].get<std::string>()));
                inventory_object.insert("quantity", value_obj["quantity"].get<int>());
                inventory_object.insert("price", value_obj["price"].get<double>());
                inventory_object.insert("currency", QString::fromStdString(value_obj["currency"].get<std::string>()));
                inventory_object.insert("condition", QString::fromStdString(value_obj["condition"].get<std::string>()));
                if(value_obj.contains("location") && value_obj["location"].is_string()) {
                    inventory_object.insert("location", QString::fromStdString(value_obj["location"].get<std::string>()));
                }
                inventory_object.insert("date", QString::fromStdString(value_obj["date"].get<std::string>()));
                if(value_obj.contains("quantity_per_order") && value_obj["quantity_per_order"].is_number_integer()) {
                    inventory_object.insert("quantity_per_order", value_obj["quantity_per_order"].get<int>());
                }
                if(value_obj.contains("payment_coins") && value_obj["payment_coins"].is_array()) {
                    const auto& payment_coins_array = value_obj["payment_coins"];//.get<std::vector<std::string>>();
                    QStringList paymentCoinsList;
                    for (const auto& payment_coin : payment_coins_array) {
                        if(payment_coin.is_string()) {
                            paymentCoinsList << QString::fromStdString(payment_coin);
                        }
                    }
                    inventory_object.insert("payment_coins", paymentCoinsList);
                }
                if(value_obj.contains("payment_options") && value_obj["payment_options"].is_array()) {
                    const auto& payment_options_array = value_obj["payment_options"];
                    QStringList paymentOptionsList;
                    for (const auto& payment_option : payment_options_array) {
                        if(payment_option.is_string()) {
                            paymentOptionsList << QString::fromStdString(payment_option);
                        }
                    }
                    inventory_object.insert("payment_options", paymentOptionsList);
                }
                if(value_obj.contains("delivery_options") && value_obj["delivery_options"].is_array()) {
                    const auto& delivery_options_array = value_obj["delivery_options"];
                    QStringList deliveryOptionsList;
                    for (const auto& delivery_option : delivery_options_array) {
                        if(delivery_option.is_string()) {
                            deliveryOptionsList << QString::fromStdString(delivery_option);
                        }
                    }
                    inventory_object.insert("delivery_options", deliveryOptionsList);
                }                
                if(value_obj.contains("shipping_options") && value_obj["shipping_options"].is_array()) {
                    const auto& shipping_options_array = value_obj["shipping_options"];
                    QStringList shippingOptionsList;
                    for (const auto& shipping_option : shipping_options_array) {
                        if(shipping_option.is_string()) {
                            shippingOptionsList << QString::fromStdString(shipping_option);
                        }
                    }
                    inventory_object.insert("shipping_options", shippingOptionsList);
                }
                if(value_obj.contains("expiration_date") && value_obj["expiration_date"].is_string()) {
                    inventory_object.insert("expiration_date", QString::fromStdString(value_obj["expiration_date"].get<std::string>()));
                }
                assert(value_obj["product"].is_object());
                const auto& product_obj = value_obj["product"];
                ////inventory_object.insert("product_uuid", QString::fromStdString(product_obj["id"].get<std::string>()));
                inventory_object.insert("product_name", QString::fromStdString(product_obj["name"].get<std::string>()));
                inventory_object.insert("product_description", QString::fromStdString(product_obj["description"].get<std::string>()));
                // product category and subcategories
                std::string category = product_obj["category"].get<std::string>();
                product_categories.append(QString::fromStdString(category));
                if (product_obj.contains("subcategories") && product_obj["subcategories"].is_array()) {
                    const auto& subcategories_array = product_obj["subcategories"];
                    for (const auto& subcategory : subcategories_array) {
                        if (subcategory.is_string()) {
                            product_categories.append(QString::fromStdString(subcategory.get<std::string>()));
                        }
                    }
                }
                inventory_object.insert("product_categories", product_categories);
                //inventory_object.insert("", QString::fromStdString(product_obj[""].get<std::string>()));
                // product attributes
                if (product_obj.contains("attributes") && product_obj["attributes"].is_array()) {
                    const auto& attributes_array = product_obj["attributes"];
                    for (const auto& attribute : attributes_array) {
                        if (attribute.is_object() && attribute.contains("weight")) { // attributes is an array of objects
                            double weight = attribute["weight"].get<double>();
                            inventory_object.insert("product_weight", weight);
                        }
                    }
                }
                // product images
                if (product_obj.contains("images") && product_obj["images"].is_array()) {
                    const auto& images_array = product_obj["images"];
                    for (const auto& image : images_array) {
                        if (image.contains("name") && image.contains("id")) {
                            const auto& image_name = image["name"].get<std::string>();
                            const auto& image_id = image["id"].get<int>();
                            
                            QVariantMap image_map;
                            image_map.insert("name", QString::fromStdString(image_name));
                            image_map.insert("id", image_id);
                            product_images.append(image_map);
                        }
                    }
                    inventory_object.insert("product_images", product_images);
                }
                if (product_obj.contains("thumbnail") && product_obj["thumbnail"].is_string()) {
                    inventory_object.insert("product_thumbnail", QString::fromStdString(product_obj["thumbnail"].get<std::string>()));
                }
                // Skip products with illicit categories/subcategories
                if (hide_illicit_items) {
                    if(isIllicitItem(inventory_object)) {
                        continue;
                    }
                }
            }
            inventory_array.append(inventory_object);
        }
    }
        
    sqlite3_finalize(stmt);

    return inventory_array;
}

QVariantList laiin::Backend::getListingsBySearchTerm(const QString& searchTerm, bool hide_illicit_items) {
    // Transition from Sqlite to DHT:
    Client * client = Client::get_main_client();
    db::Sqlite3 * database = laiin::get_database();
    if(!database) throw std::runtime_error("database is NULL");
    
    std::string command = "SELECT DISTINCT key FROM mappings WHERE (search_term MATCH ?1 OR search_term MATCH ?1 || '*') AND (content = 'listing') LIMIT ?2;";//"SELECT DISTINCT key FROM mappings WHERE (search_term MATCH ? OR search_term LIKE '%' || ? || '%' COLLATE NOCASE) AND (content MATCH 'listing');";//"SELECT DISTINCT key FROM mappings WHERE search_term MATCH ? AND content MATCH 'listing';";
    sqlite3_stmt * stmt = nullptr;
    // Prepare (compile) statement
    if(sqlite3_prepare_v2(database->get_handle(), command.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        laiin::print("sqlite3_prepare_v2: " + std::string(sqlite3_errmsg(database->get_handle())), 1);
        return {};
    }
    
    // Bind value to parameter arguments
    QByteArray searchTermByteArray = searchTerm.toUtf8(); // Convert QString to std::string equivalent
    if(sqlite3_bind_text(stmt, 1, searchTermByteArray.constData(), searchTermByteArray.length(), SQLITE_STATIC) != SQLITE_OK) {
        laiin::print("sqlite3_bind_text: " + std::string(sqlite3_errmsg(database->get_handle())), 1);
        sqlite3_finalize(stmt);
        return {};database->execute("ROLLBACK;"); return {};
    }        
    
    if(sqlite3_bind_int(stmt, 2, laiin_MAX_SEARCH_RESULTS) != SQLITE_OK) {
        laiin::print("sqlite3_bind_int: " + std::string(sqlite3_errmsg(database->get_handle())), 1);
        sqlite3_finalize(stmt);
        return {};database->execute("ROLLBACK;"); return {};
    }            
    
    // Check whether the prepared statement returns no data (for example an UPDATE)
    if(sqlite3_column_count(stmt) == 0) {
        laiin::print("No data found. Be sure to use an appropriate SELECT statement", 1);
        return {};
    }
    
    QVariantList catalog;
    // Get all table values row by row
    while(sqlite3_step(stmt) == SQLITE_ROW) {
        for(int i = 0; i < sqlite3_column_count(stmt); i++) {
            QVariantMap listing; // Create an object for each row
            QVariantList product_images;
            QStringList product_categories;
            
            std::string column_value = (sqlite3_column_text(stmt, i) == nullptr) ? "NULL" : reinterpret_cast<const char *>(sqlite3_column_text(stmt, i));//std::cout << column_value  << " (" << i << ")" << std::endl;
            if(column_value == "NULL") continue; // Skip invalid columns
            QString key = QString::fromStdString(column_value);
            // Get the value of the corresponding key from the DHT
            std::string response;
            client->get(key.toStdString(), response); // TODO: error handling
            std::cout << "Received response (get): " << response << "\n";
            // Parse the response
            nlohmann::json json = nlohmann::json::parse(response);
            if(json.contains("error")) { 
                std::string response2;
                client->remove(key.toStdString(), response2);
                std::cout << "Received response (remove): " << response2 << "\n";
                //emit categoryProductCountChanged();//(category_id);
                //emit searchResultsChanged();
                continue; // Key is lost or missing from DHT, skip to next iteration
            }
            
            const auto& response_obj = json["response"];
            assert(response_obj.is_object());
            if (response_obj.contains("value") && response_obj["value"].is_string()) {
                const auto& value = response_obj["value"].get<std::string>();
                nlohmann::json value_obj = nlohmann::json::parse(value);
                assert(value_obj.is_object());//std::cout << value_obj.dump(4) << "\n";
                std::string metadata = value_obj["metadata"].get<std::string>();
                listing.insert("key", key);
                listing.insert("listing_uuid", QString::fromStdString(value_obj["id"].get<std::string>()));
                listing.insert("seller_id", QString::fromStdString(value_obj["seller_id"].get<std::string>()));
                listing.insert("quantity", value_obj["quantity"].get<int>());
                listing.insert("price", value_obj["price"].get<double>());
                listing.insert("currency", QString::fromStdString(value_obj["currency"].get<std::string>()));
                listing.insert("condition", QString::fromStdString(value_obj["condition"].get<std::string>()));
                if(value_obj.contains("location") && value_obj["location"].is_string()) {
                    listing.insert("location", QString::fromStdString(value_obj["location"].get<std::string>()));
                }
                listing.insert("date", QString::fromStdString(value_obj["date"].get<std::string>()));
                if(value_obj.contains("quantity_per_order") && value_obj["quantity_per_order"].is_number_integer()) {
                    listing.insert("quantity_per_order", value_obj["quantity_per_order"].get<int>());
                }
                if(value_obj.contains("payment_coins") && value_obj["payment_coins"].is_array()) {
                    const auto& payment_coins_array = value_obj["payment_coins"];
                    QStringList paymentCoinsList;
                    for (const auto& payment_coin : payment_coins_array) {
                        if(payment_coin.is_string()) {
                            paymentCoinsList << QString::fromStdString(payment_coin);
                        }
                    }
                    listing.insert("payment_coins", paymentCoinsList);
                }
                if(value_obj.contains("payment_options") && value_obj["payment_options"].is_array()) {
                    const auto& payment_options_array = value_obj["payment_options"];
                    QStringList paymentOptionsList;
                    for (const auto& payment_option : payment_options_array) {
                        if(payment_option.is_string()) {
                            paymentOptionsList << QString::fromStdString(payment_option);
                        }
                    }
                    listing.insert("payment_options", paymentOptionsList);
                }
                if(value_obj.contains("delivery_options") && value_obj["delivery_options"].is_array()) {
                    const auto& delivery_options_array = value_obj["delivery_options"];
                    QStringList deliveryOptionsList;
                    for (const auto& delivery_option : delivery_options_array) {
                        if(delivery_option.is_string()) {
                            deliveryOptionsList << QString::fromStdString(delivery_option);
                        }
                    }
                    listing.insert("delivery_options", deliveryOptionsList);
                }                
                if(value_obj.contains("shipping_options") && value_obj["shipping_options"].is_array()) {
                    const auto& shipping_options_array = value_obj["shipping_options"];
                    QStringList shippingOptionsList;
                    for (const auto& shipping_option : shipping_options_array) {
                        if(shipping_option.is_string()) {
                            shippingOptionsList << QString::fromStdString(shipping_option);
                        }
                    }
                    listing.insert("shipping_options", shippingOptionsList);
                }
                if(value_obj.contains("expiration_date") && value_obj["expiration_date"].is_string()) {
                    listing.insert("expiration_date", QString::fromStdString(value_obj["expiration_date"].get<std::string>()));
                }
                assert(value_obj["product"].is_object());
                const auto& product_obj = value_obj["product"];
                ////listing.insert("product_uuid", QString::fromStdString(product_obj["id"].get<std::string>()));
                listing.insert("product_name", QString::fromStdString(product_obj["name"].get<std::string>()));
                listing.insert("product_description", QString::fromStdString(product_obj["description"].get<std::string>()));
                // product category and subcategories
                std::string category = product_obj["category"].get<std::string>();
                product_categories.append(QString::fromStdString(category));
                if (product_obj.contains("subcategories") && product_obj["subcategories"].is_array()) {
                    const auto& subcategories_array = product_obj["subcategories"];
                    for (const auto& subcategory : subcategories_array) {
                        if (subcategory.is_string()) {
                            product_categories.append(QString::fromStdString(subcategory.get<std::string>()));
                        }
                    }
                }
                listing.insert("product_categories", product_categories);
                //listing.insert("", QString::fromStdString(product_obj[""].get<std::string>()));
                // product attributes
                if (product_obj.contains("attributes") && product_obj["attributes"].is_array()) {
                    const auto& attributes_array = product_obj["attributes"];
                    for (const auto& attribute : attributes_array) {
                        if (attribute.is_object() && attribute.contains("weight")) { // attributes is an array of objects
                            double weight = attribute["weight"].get<double>();
                            listing.insert("product_weight", weight);
                        }
                    }
                }
                // product images
                if (product_obj.contains("images") && product_obj["images"].is_array()) {
                    const auto& images_array = product_obj["images"];
                    for (const auto& image : images_array) {
                        if (image.contains("name") && image.contains("id")) {
                            const auto& image_name = image["name"].get<std::string>();
                            const auto& image_id = image["id"].get<int>();
                            
                            QVariantMap image_map;
                            image_map.insert("name", QString::fromStdString(image_name));
                            image_map.insert("id", image_id);
                            product_images.append(image_map);
                        }
                    }
                    listing.insert("product_images", product_images);
                }
                // Skip products with illicit categories/subcategories
                if (hide_illicit_items) {
                    if(isIllicitItem(listing)) {
                        continue;
                    }
                }
            }
            // Append to catalog only if the key was found successfully
            catalog.append(listing);
        }
    }
    
    sqlite3_finalize(stmt);

    return catalog;
}

QVariantList laiin::Backend::getListings(int sorting, bool hide_illicit_items) {
    // Transition from Sqlite to DHT:
    Client * client = Client::get_main_client();
    db::Sqlite3 * database = laiin::get_database();
    if(!database) throw std::runtime_error("database is NULL");
    
    std::string command = "SELECT DISTINCT key FROM mappings WHERE content = 'listing';";
    sqlite3_stmt * stmt = nullptr;
    // Prepare (compile) statement
    if(sqlite3_prepare_v2(database->get_handle(), command.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        laiin::print("sqlite3_prepare_v2: " + std::string(sqlite3_errmsg(database->get_handle())), 1);
        return {};
    }
    // Check whether the prepared statement returns no data (for example an UPDATE)
    if(sqlite3_column_count(stmt) == 0) {
        laiin::print("No data found. Be sure to use an appropriate SELECT statement", 1);
        return {};
    }
    
    QVariantList catalog;
    // Get all table values row by row
    while(sqlite3_step(stmt) == SQLITE_ROW) {
        QVariantMap listing; // Create an object for each row
        QVariantList product_images;
        QStringList product_categories;

        for(int i = 0; i < sqlite3_column_count(stmt); i++) {
            std::string column_value = (sqlite3_column_text(stmt, i) == nullptr) ? "NULL" : reinterpret_cast<const char *>(sqlite3_column_text(stmt, i));//std::cout << column_value  << " (" << i << ")" << std::endl;
            if(column_value == "NULL") continue; // Skip invalid columns
            QString key = QString::fromStdString(column_value);
            // Get the value of the corresponding key from the DHT
            std::string response;
            client->get(key.toStdString(), response); // TODO: error handling
            std::cout << "Received response (get): " << response << "\n";
            // Parse the response
            nlohmann::json json = nlohmann::json::parse(response);
            if(json.contains("error")) {
                std::string response2;
                client->remove(key.toStdString(), response2);
                std::cout << "Received response (remove): " << response2 << "\n";
                //emit categoryProductCountChanged();//(category_id);
                //emit searchResultsChanged();
                continue; // Key is lost or missing from DHT, skip to next iteration
            }
            
            const auto& response_obj = json["response"];
            assert(response_obj.is_object());
            if (response_obj.contains("value") && response_obj["value"].is_string()) {
                const auto& value = response_obj["value"].get<std::string>();
                nlohmann::json value_obj = nlohmann::json::parse(value);
                assert(value_obj.is_object());//std::cout << value_obj.dump(4) << "\n";
                std::string metadata = value_obj["metadata"].get<std::string>();
                if (metadata != "listing") { std::cerr << "Invalid metadata. \"listing\" expected, got \"" << metadata << "\" instead\n"; continue; }
                listing.insert("key", key);
                listing.insert("listing_uuid", QString::fromStdString(value_obj["id"].get<std::string>()));
                listing.insert("seller_id", QString::fromStdString(value_obj["seller_id"].get<std::string>()));
                listing.insert("quantity", value_obj["quantity"].get<int>());
                listing.insert("price", value_obj["price"].get<double>());
                listing.insert("currency", QString::fromStdString(value_obj["currency"].get<std::string>()));
                listing.insert("condition", QString::fromStdString(value_obj["condition"].get<std::string>()));
                if(value_obj.contains("location") && value_obj["location"].is_string()) {
                    listing.insert("location", QString::fromStdString(value_obj["location"].get<std::string>()));
                }
                listing.insert("date", QString::fromStdString(value_obj["date"].get<std::string>()));
                if(value_obj.contains("quantity_per_order") && value_obj["quantity_per_order"].is_number_integer()) {
                    listing.insert("quantity_per_order", value_obj["quantity_per_order"].get<int>());
                }
                if(value_obj.contains("payment_coins") && value_obj["payment_coins"].is_array()) {
                    const auto& payment_coins_array = value_obj["payment_coins"];
                    QStringList paymentCoinsList;
                    for (const auto& payment_coin : payment_coins_array) {
                        if(payment_coin.is_string()) {
                            paymentCoinsList << QString::fromStdString(payment_coin);
                        }
                    }
                    listing.insert("payment_coins", paymentCoinsList);
                }
                if(value_obj.contains("payment_options") && value_obj["payment_options"].is_array()) {
                    const auto& payment_options_array = value_obj["payment_options"];
                    QStringList paymentOptionsList;
                    for (const auto& payment_option : payment_options_array) {
                        if(payment_option.is_string()) {
                            paymentOptionsList << QString::fromStdString(payment_option);
                        }
                    }
                    listing.insert("payment_options", paymentOptionsList);
                }
                if(value_obj.contains("delivery_options") && value_obj["delivery_options"].is_array()) {
                    const auto& delivery_options_array = value_obj["delivery_options"];
                    QStringList deliveryOptionsList;
                    for (const auto& delivery_option : delivery_options_array) {
                        if(delivery_option.is_string()) {
                            deliveryOptionsList << QString::fromStdString(delivery_option);
                        }
                    }
                    listing.insert("delivery_options", deliveryOptionsList);
                }                
                if(value_obj.contains("shipping_options") && value_obj["shipping_options"].is_array()) {
                    const auto& shipping_options_array = value_obj["shipping_options"];
                    QStringList shippingOptionsList;
                    for (const auto& shipping_option : shipping_options_array) {
                        if(shipping_option.is_string()) {
                            shippingOptionsList << QString::fromStdString(shipping_option);
                        }
                    }
                    listing.insert("shipping_options", shippingOptionsList);
                }
                if(value_obj.contains("expiration_date") && value_obj["expiration_date"].is_string()) {
                    listing.insert("expiration_date", QString::fromStdString(value_obj["expiration_date"].get<std::string>()));
                }
                assert(value_obj["product"].is_object());
                const auto& product_obj = value_obj["product"];
                ////listing.insert("product_uuid", QString::fromStdString(product_obj["id"].get<std::string>()));
                listing.insert("product_name", QString::fromStdString(product_obj["name"].get<std::string>()));
                listing.insert("product_description", QString::fromStdString(product_obj["description"].get<std::string>()));
                // product category and subcategories
                std::string category = product_obj["category"].get<std::string>();//int product_category_id = get_category_id_by_name(product_category_name);
                product_categories.append(QString::fromStdString(category));
                if (product_obj.contains("subcategories") && product_obj["subcategories"].is_array()) {
                    const auto& subcategories_array = product_obj["subcategories"];
                    for (const auto& subcategory : subcategories_array) {
                        if (subcategory.is_string()) {
                            product_categories.append(QString::fromStdString(subcategory.get<std::string>()));
                        }
                    }
                }
                listing.insert("product_categories", product_categories);
                //listing.insert("", QString::fromStdString(product_obj[""].get<std::string>()));
                // product attributes
                if (product_obj.contains("attributes") && product_obj["attributes"].is_array()) {
                    const auto& attributes_array = product_obj["attributes"];
                    for (const auto& attribute : attributes_array) {
                        if (attribute.is_object() && attribute.contains("weight")) { // attributes is an array of objects
                            double weight = attribute["weight"].get<double>();
                            listing.insert("product_weight", weight);
                        }
                    }
                }
                // product images
                if (product_obj.contains("images") && product_obj["images"].is_array()) {
                    const auto& images_array = product_obj["images"];
                    for (const auto& image : images_array) {
                        if (image.contains("name") && image.contains("id")) {
                            const auto& image_name = image["name"].get<std::string>();
                            const auto& image_id = image["id"].get<int>();
                            QVariantMap image_map;
                            image_map.insert("name", QString::fromStdString(image_name));
                            image_map.insert("id", image_id);
                            product_images.append(image_map);
                        }
                    }
                    listing.insert("product_images", product_images);
                }
                // product thumbnail
                if (product_obj.contains("thumbnail") && product_obj["thumbnail"].is_string()) {
                    listing.insert("product_thumbnail", QString::fromStdString(product_obj["thumbnail"].get<std::string>()));
                }
                // Skip products with illicit categories/subcategories
                if (hide_illicit_items) {
                    if(isIllicitItem(listing)) {
                        continue;
                    }
                }
            }
            catalog.append(listing);
        }
    }
    
    sqlite3_finalize(stmt);
    
    switch(sorting) {
        case static_cast<int>(EnumWrapper::Sorting::SortNone):
            // Code for sorting by none - do nothing
            break;
        case static_cast<int>(EnumWrapper::Sorting::SortByCategory):
            // Code for sorting by category - unavailable. Use getListingsByCategory() instead
            break;
        case static_cast<int>(EnumWrapper::Sorting::SortByMostRecent):
            // Perform the sorting operation on the catalog based on the "most recent" criteria
            std::sort(catalog.begin(), catalog.end(), [](const QVariant& a, const QVariant& b) {
                QVariantMap listingA = a.toMap();
                QVariantMap listingB = b.toMap();
                QString dateA = listingA["date"].toString();
                QString dateB = listingB["date"].toString();
                
                // Convert 'Z' to UTC+0 offset
                if (dateA.endsWith("Z")) {
                    dateA.replace(dateA.length() - 1, 1, "+00:00");
                }
                if (dateB.endsWith("Z")) {
                    dateB.replace(dateB.length() - 1, 1, "+00:00");
                }
                
                QDateTime dateTimeA = QDateTime::fromString(dateA, Qt::ISODateWithMs);
                QDateTime dateTimeB = QDateTime::fromString(dateB, Qt::ISODateWithMs);

                return dateTimeA > dateTimeB;
            });
            break;
        case static_cast<int>(EnumWrapper::Sorting::SortByOldest):
            std::sort(catalog.begin(), catalog.end(), [](const QVariant& a, const QVariant& b) {
                QVariantMap listingA = a.toMap();
                QVariantMap listingB = b.toMap();
                QString dateA = listingA["date"].toString();
                QString dateB = listingB["date"].toString();
                
                // Convert 'Z' to UTC+0 offset
                if (dateA.endsWith("Z")) {
                    dateA.replace(dateA.length() - 1, 1, "+00:00");
                }
                if (dateB.endsWith("Z")) {
                    dateB.replace(dateB.length() - 1, 1, "+00:00");
                }
                
                QDateTime dateTimeA = QDateTime::fromString(dateA, Qt::ISODateWithMs);
                QDateTime dateTimeB = QDateTime::fromString(dateB, Qt::ISODateWithMs);

                return dateTimeA < dateTimeB;
            });
            break;
        case static_cast<int>(EnumWrapper::Sorting::SortByAlphabeticalOrder):
            // Sort the catalog list by product name (alphabetically)
            std::sort(catalog.begin(), catalog.end(), [](const QVariant& listing1, const QVariant& listing2) {
                QString productName1 = listing1.toMap()["product_name"].toString();
                QString productName2 = listing2.toMap()["product_name"].toString();
                return QString::compare(productName1, productName2, Qt::CaseInsensitive) < 0;
            });
            break;
        case static_cast<int>(EnumWrapper::Sorting::SortByPriceLowest):
            // Perform the sorting operation on the catalog based on the "price lowest" criteria
            std::sort(catalog.begin(), catalog.end(), [](const QVariant& a, const QVariant& b) {
                QVariantMap listingA = a.toMap();
                QVariantMap listingB = b.toMap();
                return listingA["price"].toDouble() < listingB["price"].toDouble();
            });
            break;
        case static_cast<int>(EnumWrapper::Sorting::SortByPriceHighest):
            // Perform the sorting operation on the catalog based on the "price highest" criteria
            std::sort(catalog.begin(), catalog.end(), [](const QVariant& a, const QVariant& b) {
                QVariantMap listingA = a.toMap();
                QVariantMap listingB = b.toMap();
                return listingA["price"].toDouble() > listingB["price"].toDouble();
            });
            break;
        case static_cast<int>(EnumWrapper::Sorting::SortByMostFavorited):
            // Code for sorting by most favorited
            break;
        case static_cast<int>(EnumWrapper::Sorting::SortByMostSales):
            // Code for sorting by most sales
            break;
        default:
            // Code for handling unknown sorting value - do nothing
            break;
    }

    return catalog;    
}

QVariantList laiin::Backend::getListingsByCategory(int category_id, bool hide_illicit_items) {
    // Transition from Sqlite to DHT:
    Client * client = Client::get_main_client();
    db::Sqlite3 * database = laiin::get_database();
    if(!database) throw std::runtime_error("database is NULL");
    
    std::string command = "SELECT DISTINCT key FROM mappings WHERE search_term = ? AND content = 'listing';";
    sqlite3_stmt * stmt = nullptr;
    // Prepare (compile) statement
    if(sqlite3_prepare_v2(database->get_handle(), command.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        laiin::print("sqlite3_prepare_v2: " + std::string(sqlite3_errmsg(database->get_handle())), 1);
        return {};
    }
    
    std::string category = get_category_name_by_id(category_id);
    // Bind value to parameter arguments
    if(sqlite3_bind_text(stmt, 1, category.c_str(), category.length(), SQLITE_STATIC) != SQLITE_OK) {
        laiin::print("sqlite3_bind_text: " + std::string(sqlite3_errmsg(database->get_handle())), 1);
        sqlite3_finalize(stmt);
        return {};database->execute("ROLLBACK;"); return {};
    }        
    
    // Check whether the prepared statement returns no data (for example an UPDATE)
    if(sqlite3_column_count(stmt) == 0) {
        laiin::print("No data found. Be sure to use an appropriate SELECT statement", 1);
        return {};
    }
    
    QVariantList catalog;
    // Get all table values row by row
    while(sqlite3_step(stmt) == SQLITE_ROW) {
        QVariantMap listing; // Create an object for each row
        QVariantList product_images;
        QStringList product_categories;

        for(int i = 0; i < sqlite3_column_count(stmt); i++) {
            std::string column_value = (sqlite3_column_text(stmt, i) == nullptr) ? "NULL" : reinterpret_cast<const char *>(sqlite3_column_text(stmt, i));//std::cout << column_value  << " (" << i << ")" << std::endl;
            if(column_value == "NULL") continue; // Skip invalid columns
            QString key = QString::fromStdString(column_value);
            // Get the value of the corresponding key from the DHT
            std::string response;
            client->get(key.toStdString(), response); // TODO: error handling
            std::cout << "Received response (get): " << response << "\n";
            // Parse the response
            nlohmann::json json = nlohmann::json::parse(response);
            if(json.contains("error")) {
                std::string response2;
                client->remove(key.toStdString(), response2);
                std::cout << "Received response (remove): " << response2 << "\n";
                //emit categoryProductCountChanged();//(category_id);
                //emit searchResultsChanged();
                continue; // Key is lost or missing from DHT, skip to next iteration
            }
            
            const auto& response_obj = json["response"];
            assert(response_obj.is_object());
            if (response_obj.contains("value") && response_obj["value"].is_string()) {
                const auto& value = response_obj["value"].get<std::string>();
                nlohmann::json value_obj = nlohmann::json::parse(value);
                assert(value_obj.is_object());//std::cout << value_obj.dump(4) << "\n";
                std::string metadata = value_obj["metadata"].get<std::string>();
                if (metadata != "listing") { std::cerr << "Invalid metadata. \"listing\" expected, got \"" << metadata << "\" instead\n"; continue; }
                listing.insert("key", key);
                listing.insert("listing_uuid", QString::fromStdString(value_obj["id"].get<std::string>()));
                listing.insert("seller_id", QString::fromStdString(value_obj["seller_id"].get<std::string>()));
                listing.insert("quantity", value_obj["quantity"].get<int>());
                listing.insert("price", value_obj["price"].get<double>());
                listing.insert("currency", QString::fromStdString(value_obj["currency"].get<std::string>()));
                listing.insert("condition", QString::fromStdString(value_obj["condition"].get<std::string>()));
                if(value_obj.contains("location") && value_obj["location"].is_string()) {
                    listing.insert("location", QString::fromStdString(value_obj["location"].get<std::string>()));
                }
                listing.insert("date", QString::fromStdString(value_obj["date"].get<std::string>()));
                if(value_obj.contains("quantity_per_order") && value_obj["quantity_per_order"].is_number_integer()) {
                    listing.insert("quantity_per_order", value_obj["quantity_per_order"].get<int>());
                }
                if(value_obj.contains("payment_coins") && value_obj["payment_coins"].is_array()) {
                    const auto& payment_coins_array = value_obj["payment_coins"];
                    QStringList paymentCoinsList;
                    for (const auto& payment_coin : payment_coins_array) {
                        if(payment_coin.is_string()) {
                            paymentCoinsList << QString::fromStdString(payment_coin);
                        }
                    }
                    listing.insert("payment_coins", paymentCoinsList);
                }
                if(value_obj.contains("payment_options") && value_obj["payment_options"].is_array()) {
                    const auto& payment_options_array = value_obj["payment_options"];
                    QStringList paymentOptionsList;
                    for (const auto& payment_option : payment_options_array) {
                        if(payment_option.is_string()) {
                            paymentOptionsList << QString::fromStdString(payment_option);
                        }
                    }
                    listing.insert("payment_options", paymentOptionsList);
                }
                if(value_obj.contains("delivery_options") && value_obj["delivery_options"].is_array()) {
                    const auto& delivery_options_array = value_obj["delivery_options"];
                    QStringList deliveryOptionsList;
                    for (const auto& delivery_option : delivery_options_array) {
                        if(delivery_option.is_string()) {
                            deliveryOptionsList << QString::fromStdString(delivery_option);
                        }
                    }
                    listing.insert("delivery_options", deliveryOptionsList);
                }                
                if(value_obj.contains("shipping_options") && value_obj["shipping_options"].is_array()) {
                    const auto& shipping_options_array = value_obj["shipping_options"];
                    QStringList shippingOptionsList;
                    for (const auto& shipping_option : shipping_options_array) {
                        if(shipping_option.is_string()) {
                            shippingOptionsList << QString::fromStdString(shipping_option);
                        }
                    }
                    listing.insert("shipping_options", shippingOptionsList);
                }
                if(value_obj.contains("expiration_date") && value_obj["expiration_date"].is_string()) {
                    listing.insert("expiration_date", QString::fromStdString(value_obj["expiration_date"].get<std::string>()));
                }
                assert(value_obj["product"].is_object());
                const auto& product_obj = value_obj["product"];
                ////listing.insert("product_uuid", QString::fromStdString(product_obj["id"].get<std::string>()));
                listing.insert("product_name", QString::fromStdString(product_obj["name"].get<std::string>()));
                listing.insert("product_description", QString::fromStdString(product_obj["description"].get<std::string>()));
                // product category and subcategories
                std::string category = product_obj["category"].get<std::string>();
                product_categories.append(QString::fromStdString(category));
                if (product_obj.contains("subcategories") && product_obj["subcategories"].is_array()) {
                    const auto& subcategories_array = product_obj["subcategories"];
                    for (const auto& subcategory : subcategories_array) {
                        if (subcategory.is_string()) {
                            product_categories.append(QString::fromStdString(subcategory.get<std::string>()));
                        }
                    }
                }
                listing.insert("product_categories", product_categories);
                //listing.insert("", QString::fromStdString(product_obj[""].get<std::string>()));
                // product attributes
                if (product_obj.contains("attributes") && product_obj["attributes"].is_array()) {
                    const auto& attributes_array = product_obj["attributes"];
                    for (const auto& attribute : attributes_array) {
                        if (attribute.is_object() && attribute.contains("weight")) { // attributes is an array of objects
                            double weight = attribute["weight"].get<double>();
                            listing.insert("product_weight", weight);
                        }
                    }
                }
                //listing.insert("code", QString::fromStdString(product_obj[""].get<std::string>()));
                //listing.insert("tags", QString::fromStdString(product_obj[""].get<std::string>()));
                // product images
                if (product_obj.contains("images") && product_obj["images"].is_array()) {
                    const auto& images_array = product_obj["images"];
                    for (const auto& image : images_array) {
                        if (image.contains("name") && image.contains("id")) {
                            const auto& image_name = image["name"].get<std::string>();
                            const auto& image_id = image["id"].get<int>();//source,data, etc.
                            
                            QVariantMap image_map;
                            image_map.insert("name", QString::fromStdString(image_name));
                            image_map.insert("id", image_id);
                            product_images.append(image_map);
                        }
                    }
                    listing.insert("product_images", product_images);
                }
                // Skip products with illicit categories/subcategories
                if (hide_illicit_items) {
                    if(isIllicitItem(listing)) {
                        continue;
                    }
                }
            }
            catalog.append(listing);
        }
    }
    
    sqlite3_finalize(stmt);

    return catalog;
}

QVariantList laiin::Backend::getListingsByMostRecent(int limit, bool hide_illicit_items) {
    auto catalog = getListings(static_cast<int>(EnumWrapper::Sorting::SortByMostRecent), hide_illicit_items);
    if (catalog.size() > limit) {
        catalog = catalog.mid(0, limit);
    }
    return catalog;
}

QVariantList laiin::Backend::sortBy(const QVariantList& catalog, int sorting) {
    // Make a copy of the catalog to work with
    QVariantList sortedCatalog = catalog;
    
    switch(sorting) {
        case static_cast<int>(EnumWrapper::Sorting::SortNone):
            // Code for sorting by none - do nothing
            break;
        case static_cast<int>(EnumWrapper::Sorting::SortByCategory):
            // Code for sorting by category - unavailable. Use getListingsByCategory() instead
            break;
        case static_cast<int>(EnumWrapper::Sorting::SortByMostRecent):
            // Perform the sorting operation on the catalog based on the "most recent" criteria
            std::sort(sortedCatalog.begin(), sortedCatalog.end(), [](const QVariant& a, const QVariant& b) {
                QVariantMap listingA = a.toMap();
                QVariantMap listingB = b.toMap();
                QString dateA = listingA["date"].toString();
                QString dateB = listingB["date"].toString();
                
                // Convert 'Z' to UTC+0 offset
                if (dateA.endsWith("Z")) {
                    dateA.replace(dateA.length() - 1, 1, "+00:00");
                }
                if (dateB.endsWith("Z")) {
                    dateB.replace(dateB.length() - 1, 1, "+00:00");
                }
                
                QDateTime dateTimeA = QDateTime::fromString(dateA, Qt::ISODateWithMs);
                QDateTime dateTimeB = QDateTime::fromString(dateB, Qt::ISODateWithMs);

                return dateTimeA > dateTimeB;
            });
            break;
        case static_cast<int>(EnumWrapper::Sorting::SortByOldest):
            std::sort(sortedCatalog.begin(), sortedCatalog.end(), [](const QVariant& a, const QVariant& b) {
                QVariantMap listingA = a.toMap();
                QVariantMap listingB = b.toMap();
                QString dateA = listingA["date"].toString();
                QString dateB = listingB["date"].toString();
                
                // Convert 'Z' to UTC+0 offset
                if (dateA.endsWith("Z")) {
                    dateA.replace(dateA.length() - 1, 1, "+00:00");
                }
                if (dateB.endsWith("Z")) {
                    dateB.replace(dateB.length() - 1, 1, "+00:00");
                }
                
                QDateTime dateTimeA = QDateTime::fromString(dateA, Qt::ISODateWithMs);
                QDateTime dateTimeB = QDateTime::fromString(dateB, Qt::ISODateWithMs);

                return dateTimeA < dateTimeB;
            });
            break;
        case static_cast<int>(EnumWrapper::Sorting::SortByAlphabeticalOrder):
            // Sort the catalog list by product name (alphabetically)
            std::sort(sortedCatalog.begin(), sortedCatalog.end(), [](const QVariant& listing1, const QVariant& listing2) {
                QString productName1 = listing1.toMap()["product_name"].toString();
                QString productName2 = listing2.toMap()["product_name"].toString();
                return QString::compare(productName1, productName2, Qt::CaseInsensitive) < 0;
            });
            break;
        case static_cast<int>(EnumWrapper::Sorting::SortByPriceLowest):
            // Perform the sorting operation on the catalog based on the "price lowest" criteria
            std::sort(sortedCatalog.begin(), sortedCatalog.end(), [](const QVariant& a, const QVariant& b) {
                QVariantMap listingA = a.toMap();
                QVariantMap listingB = b.toMap();
                return listingA["price"].toDouble() < listingB["price"].toDouble();
            });
            break;
        case static_cast<int>(EnumWrapper::Sorting::SortByPriceHighest):
            // Perform the sorting operation on the catalog based on the "price highest" criteria
            std::sort(sortedCatalog.begin(), sortedCatalog.end(), [](const QVariant& a, const QVariant& b) {
                QVariantMap listingA = a.toMap();
                QVariantMap listingB = b.toMap();
                return listingA["price"].toDouble() > listingB["price"].toDouble();
            });
            break;
        case static_cast<int>(EnumWrapper::Sorting::SortByAverageRating):
            // Code for sorting by average rating
            break;    
        case static_cast<int>(EnumWrapper::Sorting::SortByMostFavorited):
            // Code for sorting by most favorited
            break;
        case static_cast<int>(EnumWrapper::Sorting::SortByMostSales):
            // Code for sorting by most sales
            break;
        default:
            // Code for handling unknown sorting value - do nothing
            break;
    }
    
    return sortedCatalog;
}


bool laiin::Backend::isIllicitItem(const QVariantMap& listing_obj) {
    std::string category_name = predefined_categories[25].name;
    
    if (!listing_obj.contains("product_categories")) {
        std::cerr << "No product categories found\n";
        return false;
    }
    
    QStringList product_categories = listing_obj["product_categories"].toStringList();
    if(product_categories.contains(QString::fromStdString(category_name))) {
        std::cout << listing_obj["product_name"].toString().toStdString() << " contains illicit content so it has been excluded from listings" << "\n";
        return true;
    }
    return false;
}


void laiin::Backend::createOrder(UserController * user_controller, const QString& shipping_address) {
    user_controller->createOrder(shipping_address);
}


QVariantList laiin::Backend::getNodeListDefault(const QString& coin) const {
    QVariantList node_list;
    std::string network_type = Wallet::get_network_type_as_string();
    std::vector<std::string> node_table = laiin::Script::get_table_string(laiin::lua_state, coin.toStdString() + ".nodes." + network_type); // Get monero nodes from settings.lua////std::cout << "lua_query: " << coin.toStdString() + ".nodes." + network_type << std::endl;
    for(auto strings : node_table) {
        node_list << QString::fromStdString(strings);
    }
    return node_list;
}

bool containsSubstring(const std::string& str, const std::vector<std::string>& substrings) {
    // Iterate over the substrings vector
    for (const auto& substring : substrings) {
        // Check if the string contains the current substring
        if (str.find(substring) != std::string::npos) {
            return true; // Substring found in the string
        }
    }
    return false; // Substring not found in the string
}

QVariantList laiin::Backend::getNodeList(const QString& coin) const {
    const QUrl url(QStringLiteral("https://monero.fail/health.json"));
    QVariantList node_list;
    QString coin_lower = coin.toLower(); // make coin name lowercase
    
    WalletNetworkType network_type = Wallet::get_network_type();
    auto network_ports = WalletNetworkPortMap[network_type];
    
    QNetworkAccessManager manager;
    QEventLoop loop;
    QObject::connect(&manager, &QNetworkAccessManager::finished, &loop, &QEventLoop::quit);

    auto reply = manager.get(QNetworkRequest(url));
    loop.exec();
    QJsonParseError error;
    const auto json_doc = QJsonDocument::fromJson(reply->readAll(), &error);
    // Use fallback monero node list if we fail to get the nodes from the url
    if (error.error != QJsonParseError::NoError) {
        laiin::print("Error reading json from " + url.toString().toStdString() + "\nUsing default nodes as fallback", 1);
        return getNodeListDefault(coin_lower);
    }
    // Get monero nodes from the JSON
    QJsonObject root_obj = json_doc.object(); // {}
    QJsonObject coin_obj = root_obj.value(coin_lower).toObject(); // "monero": {} // "wownero": {}
    QJsonObject clearnet_obj = coin_obj.value("clear").toObject(); // "clear": {} // "onion": {}, "web_compatible": {}
    // Loop through monero nodes (clearnet)
    foreach(const QString& key, clearnet_obj.keys()) {//for (const auto monero_nodes : clearnet_obj) {
        QJsonObject monero_node_obj = clearnet_obj.value(key).toObject();//QJsonObject monero_node_obj = monero_nodes.toObject();
        QVariantMap node_object; // Create an object for each row
        if(containsSubstring(key.toStdString(), network_ports)) {
            node_object.insert("address", key);
            node_object.insert("available", monero_node_obj.value("available").toBool());//std::cout << "available: " << monero_node_obj.value("available").toBool() << "\n";
            ////node_object.insert("", );//////std::cout << ": " << monero_node_obj.value("checks").toArray() << "\n";
            node_object.insert("datetime_checked", monero_node_obj.value("datetime_checked").toString());//std::cout << "datetime_checked: " << monero_node_obj.value("datetime_checked").toString().toStdString() << "\n";
            node_object.insert("datetime_entered", monero_node_obj.value("datetime_entered").toString());//std::cout << "datetime_entered: " << monero_node_obj.value("datetime_entered").toString().toStdString() << "\n";
            node_object.insert("datetime_failed", monero_node_obj.value("datetime_failed").toString());//std::cout << "datetime_failed: " << monero_node_obj.value("datetime_failed").toString().toStdString() << "\n";
            node_object.insert("last_height", monero_node_obj.value("last_height").toInt());//std::cout << "last_height: " << monero_node_obj.value("last_height").toInt() << "\n";
            node_list.append(node_object); // Add node object to the node list
        }
    }
    return node_list;
}

bool laiin::Backend::isWalletDaemonRunning() const {
    int monerod = Process::get_process_by_name("monerod");
    if(monerod == -1) { return false; }
    std::cout << "\033[1;90;49m" << "monerod is running (ID:" << monerod << ")\033[0m" << std::endl; 
    return true;*/
    
    #ifdef Q_OS_WIN
    QString program = "monerod.exe";
    #else
    QString program = "monerod";
    #endif
    
    QProcess process;
    process.start("pgrep", QStringList() << program); // specific to Linux-based systems
    process.waitForFinished();

    if(process.exitCode() == 0) std::cout << "\033[1;90;49m" << program.toStdString() << " was already running in the background\033[0m" << std::endl;
    return process.exitCode() == 0;
}

QVariantList laiin::Backend::validateDisplayName(const QString& display_name) const {
    // username (will appear only in lower-case letters within the app)
    std::string username = display_name.toStdString();
    // Empty display names are acceptable
    if(display_name.isEmpty()) return { true, "" };

    if(!laiin::string_tools::is_valid_username(username)) {
        std::string default_message = "Invalid username: " + username;
        laiin::print(default_message, 1);
        if (username.length() < laiin_MIN_USERNAME_LENGTH) {
            std::string message = std::string("Display name must be at least " + std::to_string(laiin_MIN_USERNAME_LENGTH) + " characters in length");
            return { false, QString::fromStdString(message) };
        }
        if (username.length() > laiin_MAX_USERNAME_LENGTH) {
            std::string message = std::string("Display name cannot exceed " + std::to_string(laiin_MAX_USERNAME_LENGTH) + " characters in length");
            return { false, QString::fromStdString(message) };
        }
        if (std::regex_search(username, std::regex("\\s"))) {
            std::string message = "Display name cannot contain spaces\n";
            return { false, QString::fromStdString(message) };
        }
        if (!std::regex_search(username, std::regex("^[a-zA-Z]"))) {
            std::string message = "Display name must begin with a letter (cannot start with a symbol or number)";
            return { false, QString::fromStdString(message) };
        }
        if (!std::regex_search(username, std::regex("[a-zA-Z0-9]$"))) {
            std::string message = "Display name must end with a letter or number (cannot end with a symbol)";
            return { false, QString::fromStdString(message) };
        }
        if (std::regex_search(username, std::regex("[^a-zA-Z0-9._-]"))) {
            std::string message = "Display name contains invalid symbol(s) (only '.', '_', and '-' are allowed in between the display name)";
            return { false, QString::fromStdString(message) };
        }
        
        return { false, QString::fromStdString(default_message) };
    }

    return { true, "" };
}

QVariantList laiin::Backend::registerUser(WalletController* wallet_controller, const QString& display_name, UserController * user_controller, const QVariantMap& avatarMap) {
    // Make sure daemon is connected first
    if(!DaemonManager::isDaemonServerBound()) {
        return { false, "Please wait for the local daemon IPC server to connect first" };
    }
    //
    db::Sqlite3 * database = laiin::get_user_database();
    if(!database) throw std::runtime_error("database is NULL");
    //
    // Validate display name
    auto name_validation_result = validateDisplayName(display_name);
    if(!name_validation_result[0].toBool()) {
        bool boolean_result = name_validation_result[0].toBool();
        QString message_result = name_validation_result[1].toString();
        return { boolean_result, message_result };
    }
    //
    // Get wallet primary address and check its validity
    std::string primary_address = wallet_controller->getPrimaryAddress().toStdString();//laiin::print("Primary address: \033[1;33m" + primary_address + "\033[1;37m\n");
    if(!wallet_controller->getWallet()->is_valid_address(primary_address)) {
        return { false, "Invalid monero address" };
    }
    //
    // Generate RSA key pair (this is for sending/receiving encrypted messages)
    std::string config_path = laiin_DEFAULT_KEYS_PATH;
    std::string public_key_filename = config_path + "/" + primary_address + ".pub";
    std::string private_key_filename = config_path + "/" + primary_address + ".key";
    EVP_PKEY * pkey = laiin::crypto::rsa_generate_keys_get();
    if(pkey == nullptr) {
        return { false, "Failed to generate RSA key pair" };
    }
    // Get a copy of the public key
    std::string public_key = laiin::crypto::rsa_get_public_key(pkey);
    std::string private_key = laiin::crypto::rsa_get_private_key(pkey);
    // Save the key pair to disk
    if(!laiin::crypto::rsa_save_keys(pkey, public_key_filename, private_key_filename)) {
        return { false, "Failed to save RSA key pair" };
    }
    // initialize user obj
    std::unique_ptr<laiin::User> seller(laiin::Seller::on_login(*wallet_controller->getWallet()));
    user_controller->_user = std::move(seller);
    if (user_controller->getUser() == nullptr) {
        return {false, "user is NULL"};
    }
    user_controller->_user->set_name(display_name.toStdString());
    user_controller->_user->set_public_key(public_key);
    user_controller->_user->set_private_key(private_key);
    if(!avatarMap.isEmpty()) {
        Image image;
        std::vector<std::string> pieces;
        std::vector<unsigned char> data;
        
        if(avatarMap.contains("name")) image.name = avatarMap.value("name").toString().toStdString();
        if(avatarMap.contains("size")) image.size = avatarMap.value("size").toInt();
        if(avatarMap.contains("id")) image.id = avatarMap.value("id").toInt();
        if(avatarMap.contains("source")) image.source = avatarMap.value("source").toString().toStdString();
        if(avatarMap.contains("pieces") && avatarMap.value("pieces").canConvert<QStringList>()) {
            QStringList piecesList = avatarMap.value("pieces").toStringList();
            for(const QString& pieceHashStr : piecesList) {
                pieces.push_back(pieceHashStr.toStdString());
            }
            image.pieces = pieces;
        }
        if(avatarMap.contains("piece_size")) image.piece_size = avatarMap.value("piece_size").toInt();
        if(avatarMap.contains("data") && avatarMap.value("data").canConvert<QByteArray>()) {
            QByteArray imageData = avatarMap.value("data").toByteArray();
            data.reserve(imageData.size());  // Reserve space to avoid reallocations
            for (int i = 0; i < imageData.size(); ++i) {
                data.push_back(static_cast<unsigned char>(imageData.at(i)));
            }
            image.data = data;
        }
        if(avatarMap.contains("width")) image.width = avatarMap.value("width").toInt();
        if(avatarMap.contains("height")) image.height = avatarMap.value("height").toInt();
    
        user_controller->_user->avatar = std::make_unique<Image>(std::move(image));
    }

    // Store login credentials in DHT
    Client * client = Client::get_main_client();
    // If client is not connect, return error
    if (!client->is_connected()) return { false, "Not connected to local daemon IPC server" };
    // Serialize user object
    auto data = Serializer::serialize(*user_controller->_user);
    std::string key = data.first;
    std::string value = data.second;
    
    // Send put and receive response
    std::string response;
    client->put(key, value, response);
    std::cout << "Received response (put): " << response << "\n";
    //
    // Create cart for user
    QString cart_uuid = QUuid::createUuid().toString();
    cart_uuid = cart_uuid.remove("{").remove("}"); // remove brackets
    database->execute_params("INSERT INTO cart (uuid, user_id) VALUES ($1, $2)", { cart_uuid.toStdString(), user_controller->_user->get_id() });
    // Set cart id
    user_controller->_user->get_cart()->set_id(cart_uuid.toStdString());
    //
    emit user_controller->userChanged();
    emit user_controller->userLogged();
    // Display registration message
    laiin::print(((!display_name.isEmpty()) ? "Welcome to laiin, " : "Welcome to laiin") + display_name.toStdString(), 4);
    return { true, QString::fromStdString(key) };
}

int laiin::Backend::loginWithWalletFile(WalletController* wallet_controller, const QString& path, const QString& password, UserController * user_controller) { 
    db::Sqlite3 * database = laiin::get_database();
    if(!database) throw std::runtime_error("database is NULL");
    
    // Make sure daemon is connected first
    if(!DaemonManager::isDaemonServerBound()) {
        laiin::print("Please wait for the local daemon IPC server to connect first", 1);
        return static_cast<int>(EnumWrapper::LoginError::DaemonIsNotConnected);
    }    
    // Open wallet file
    std::packaged_task<int(void)> open_wallet_task([wallet_controller, path, password]() -> int {
        int wallet_error = wallet_controller->open(path, password);
        if(wallet_error != 0) {
            if(wallet_error == static_cast<int>(WalletError::WrongPassword))
                return static_cast<int>(EnumWrapper::LoginError::WrongPassword);
            if(wallet_error == static_cast<int>(WalletError::IsOpenedByAnotherProgram))
                return static_cast<int>(EnumWrapper::LoginError::WalletIsOpenedByAnotherProgram);
            if(wallet_error == static_cast<int>(WalletError::DoesNotExist))
                return static_cast<int>(EnumWrapper::LoginError::WalletDoesNotExist);
            if(wallet_error == static_cast<int>(WalletError::BadNetworkType))
                return static_cast<int>(EnumWrapper::LoginError::WalletBadNetworkType);
            if(wallet_error == static_cast<int>(WalletError::IsNotOpened))
                return static_cast<int>(EnumWrapper::LoginError::WalletIsNotOpened);
            if(wallet_error == static_cast<int>(WalletError::BadWalletType))
                return static_cast<int>(EnumWrapper::LoginError::WalletBadWalletType);    
        }
        return static_cast<int>(EnumWrapper::LoginError::Ok);
    });
    std::future<int> future_result = open_wallet_task.get_future();
    // move the task (function) to a separate thread to prevent blocking of the main thread
    std::thread worker(std::move(open_wallet_task));
    worker.detach(); // join may block but detach won't
    int login_error = future_result.get();
    if(login_error != 0) return login_error;
    // Get the primary address
    std::string primary_address = wallet_controller->getPrimaryAddress().toStdString();
    
    // Check database to see if user key (hash of primary address) exists
    bool user_found = database->get_integer_params("SELECT EXISTS(SELECT * FROM mappings WHERE search_term = ?1 AND content = 'user')", { primary_address });
    // If user key is not found in the database, then create one. This is like registering for an account
    if(!user_found) {
        // In reality, this function will return false if user key is not registered in the database
        laiin::print("Account not found in database. Please try again or register", 1);
        wallet_controller->close();
        return static_cast<int>(EnumWrapper::LoginError::UserNotFound);
    }
    // Get the account DHT key
    std::string user_key = database->get_text_params("SELECT key FROM mappings WHERE search_term = ?1 AND content = 'user'", { primary_address });
    // Save user information in memory
    std::string display_name = database->get_text_params("SELECT search_term FROM mappings WHERE key = ?1 AND LENGTH(search_term) <= 30 AND content = 'user'", { user_key });
    std::unique_ptr<laiin::User> seller(laiin::Seller::on_login(*wallet_controller->getWallet()));
    user_controller->_user = std::move(seller);
    if(user_controller->getUser() == nullptr) {
        return static_cast<int>(EnumWrapper::LoginError::UserIsNullPointer);
    }
    
    // Load RSA keys from file
    std::string config_path = laiin_DEFAULT_KEYS_PATH;
    std::string public_key_path = config_path + "/" + (primary_address + ".pub");
    std::string private_key_path = config_path + "/" + (primary_address + ".key");
    
    // Load public_key (optional)
    std::ifstream public_key_file(public_key_path);
    if (public_key_file) {
        std::ostringstream buffer;
        buffer << public_key_file.rdbuf();
        std::string public_key = buffer.str();
        user_controller->_user->set_public_key(public_key);
    }
    
    // Load private_key (mandatory)
    std::ifstream private_key_file(private_key_path);
    if (!private_key_file) {
        throw std::runtime_error("Failed to open private key file: " + private_key_path);
    }
    std::ostringstream buffer;
    buffer << private_key_file.rdbuf();
    std::string private_key = buffer.str();    
    user_controller->_user->set_private_key(private_key); // Set RSA private key
    
    emit user_controller->userChanged();
    emit user_controller->userLogged();
    // Display message
    laiin::print("Welcome back, user " + ((!display_name.empty()) ? (display_name + " (id: " + primary_address + ")") : primary_address), 4);
    return static_cast<int>(EnumWrapper::LoginError::Ok);
}

int laiin::Backend::loginWithMnemonic(WalletController* wallet_controller, const QString& mnemonic, unsigned int restore_height, UserController * user_controller) {
    db::Sqlite3 * database = laiin::get_database();
    if(!database) throw std::runtime_error("database is NULL");
    
    // Make sure daemon is connected first
    if(!DaemonManager::isDaemonServerBound()) {
        laiin::print("Please wait for the local daemon IPC server to connect first", 1);
        return static_cast<int>(EnumWrapper::LoginError::DaemonIsNotConnected);
    }
    // Initialize monero wallet with existing wallet mnemonic
    std::packaged_task<int(void)> restore_wallet_task([wallet_controller, mnemonic, restore_height]() -> int {
        int wallet_error = wallet_controller->restoreFromSeed(mnemonic, restore_height);
        if(wallet_error != 0) {
            if(wallet_error == static_cast<int>(WalletError::IsOpenedByAnotherProgram))
                return static_cast<int>(EnumWrapper::LoginError::WalletIsOpenedByAnotherProgram);
            if(wallet_error == static_cast<int>(WalletError::InvalidMnemonic))
                return static_cast<int>(EnumWrapper::LoginError::WalletInvalidMnemonic);
            if(wallet_error == static_cast<int>(WalletError::BadNetworkType))
                return static_cast<int>(EnumWrapper::LoginError::WalletBadNetworkType);
            if(wallet_error == static_cast<int>(WalletError::IsNotOpened))
                return static_cast<int>(EnumWrapper::LoginError::WalletIsNotOpened);
            if(wallet_error == static_cast<int>(WalletError::BadWalletType))
                return static_cast<int>(EnumWrapper::LoginError::WalletBadWalletType);    
        }
        return static_cast<int>(EnumWrapper::LoginError::Ok);
    });
    std::future<int> future_result = restore_wallet_task.get_future();
    // move the task (function) to a separate thread to prevent blocking of the main thread
    std::thread worker(std::move(restore_wallet_task));
    worker.detach();
    int login_error = future_result.get();
    if(login_error != 0) return login_error;
    
    // Get the primary address
    std::string primary_address = wallet_controller->getPrimaryAddress().toStdString();
    // Check database to see if user key (hash of primary address) exists
    bool user_found = database->get_integer_params("SELECT EXISTS(SELECT * FROM mappings WHERE search_term = ?1 AND content = 'user')", { primary_address });
    // If user key is not found in the database, then create one. This is like registering for an account
    if(!user_found) {
        // In reality, this function will return false if user key is not registered in the database
        laiin::print("user key not found in database. Please try again or register", 1);
        wallet_controller->close();
        return static_cast<int>(EnumWrapper::LoginError::UserNotFound);
    }
    // Get the account DHT key
    std::string user_key = database->get_text_params("SELECT key FROM mappings WHERE search_term = ?1 AND content = 'user'", { primary_address });
    // Save user information in memory
    std::string display_name = database->get_text_params("SELECT search_term FROM mappings WHERE key = ?1 AND LENGTH(search_term) <= 30 AND content = 'user'", { user_key });
    std::unique_ptr<laiin::User> seller(laiin::Seller::on_login(*wallet_controller->getWallet()));
    user_controller->_user = std::move(seller);
    if(user_controller->getUser() == nullptr) {
        return static_cast<int>(EnumWrapper::LoginError::UserIsNullPointer);
    }
    
    // Load RSA keys from file
    std::string config_path = laiin_DEFAULT_KEYS_PATH;
    std::string public_key_path = config_path + "/" + (primary_address + ".pub");
    std::string private_key_path = config_path + "/" + (primary_address + ".key");
    
    // Load public_key (optional)
    std::ifstream public_key_file(public_key_path);
    if (public_key_file) {
        std::ostringstream buffer;
        buffer << public_key_file.rdbuf();
        std::string public_key = buffer.str();
        user_controller->_user->set_public_key(public_key);
    }
    
    // Load private_key (mandatory)
    std::ifstream private_key_file(private_key_path);
    if (!private_key_file) {
        throw std::runtime_error("Failed to open private key file: " + private_key_path);
    }
    std::ostringstream buffer;
    buffer << private_key_file.rdbuf();
    std::string private_key = buffer.str();    
    user_controller->_user->set_private_key(private_key); // Set RSA private key
    
    // Emit signals
    emit user_controller->userChanged();
    emit user_controller->userLogged();

    // Display message
    laiin::print("Welcome back, user " + ((!display_name.empty()) ? (display_name + " (id: " + primary_address + ")") : primary_address), 4);
    return static_cast<int>(EnumWrapper::LoginError::Ok);
}

int laiin::Backend::loginWithKeys(WalletController* wallet_controller, UserController * user_controller) {

    db::Sqlite3 * database = laiin::get_database();
    if(!database) throw std::runtime_error("database is NULL");
    // Get the wallet from the wallet controller
    laiin::Wallet * wallet = wallet_controller->getWallet();
    // Initialize monero wallet with existing wallet mnemonic
    std::string primary_address;
    std::string secret_view_key;
    std::string secret_spend_key;
    std::cout << "Please enter your primary address:\n";
    std::getline(std::cin, primary_address);
    std::cout << "Please enter your secret view key:\n";
    std::getline(std::cin, secret_view_key);
    std::cout << "Please enter your secret spend key (optional):\n";
    std::getline(std::cin, secret_spend_key);
    // todo: allow user to specify a custom location for the wallet keyfile or use a default location
    wallet_controller->restoreFromKeys(primary_address, secret_view_key, secret_spend_key);
    // Get the hash of the primary address
    std::string user_auth_key;// = laiin::algo::sha256(primary_address);
    ////Validator::generate_sha256_hash(primary_address, user_auth_key); // temp
    laiin::print("Primary address: \033[1;33m" + primary_address + "\033[1;37m\nSHA256 hash: " + user_auth_key);
    //$ echo -n "528qdm2pXnYYesCy5VdmBneWeaSZutEijFVAKjpVHeVd4unsCSM55CjgViQsK9WFNHK1eZgcCuZ3fRqYpzKDokqSKp4yp38" | sha256sum
    // Check database to see if user key (hash of primary address) exists
    bool user_key_found = database->get_integer_params("SELECT EXISTS(SELECT * FROM users WHERE key = $1)", { user_auth_key });
    // If user key is not found in the database, then create one. This is like registering for an account
    if(!user_key_found) {
        // In reality, this function will return false if user key is not registered in the database
        laiin::print("user key not found in database. Please try again or register", 1);
        wallet_controller->close();
        return false;
    }
    // Save user information in memory
    int user_id = database->get_integer_params("SELECT id FROM users WHERE key = $1", { user_auth_key });
    // Display message
    std::string display_name = database->get_text_params("SELECT name FROM users WHERE monero_address = $1", { primary_address });
    laiin::print("Welcome back, user " + ((!display_name.empty()) ? (display_name + " (id: " + primary_address + ")") : primary_address), 4);
    return true;

    return false;
}

int laiin::Backend::loginWithHW(WalletController* wallet_controller, UserController * user_controller) {
    return false;
}


QString laiin::Backend::getPaymentCoinAsString(int paymentCoin) {
    return QString::fromStdString(get_payment_coin_as_string(static_cast<PaymentCoin>(paymentCoin)));
}

QString laiin::Backend::getShippingOptionAsString(int shippingOption) {
    return QString::fromStdString(get_shipping_option_as_string(static_cast<ShippingOption>(shippingOption)));
}


QVariantMap laiin::Backend::getNetworkStatus() const {
    if(!DaemonManager::isDaemonServerBound()) { return {}; }
    
    Client * client = Client::get_main_client();
    std::string response;
    client->get("status", response);//std::cout << "Received response (get): " << response << "\n";
    
    // Parse the response
    nlohmann::json json = nlohmann::json::parse(response);
    if(json.contains("error")) {
        return {};
    }
    
    QVariantMap network_status;
            
    const auto& response_obj = json["response"];
    assert(response_obj.is_object());
    if (response_obj.contains("connected_peers") && response_obj["connected_peers"].is_number_integer()) {
        int connected_peers = response_obj["connected_peers"].get<int>();
        network_status["connected_peers"] = connected_peers;
    }
    
    if (response_obj.contains("active_peers") && response_obj["active_peers"].is_number_integer()) {
        int active_peers = response_obj["active_peers"].get<int>();
        network_status["active_peers"] = active_peers;
    }
    
    if (response_obj.contains("idle_peers") && response_obj["idle_peers"].is_number_integer()) {
        int idle_peers = response_obj["idle_peers"].get<int>();
        network_status["idle_peers"] = idle_peers;
    }
    
    if (response_obj.contains("data_count") && response_obj["data_count"].is_number_integer()) {
        int data_count = response_obj["data_count"].get<int>();
        network_status["data_count"] = data_count;
    }
    
    if (response_obj.contains("data_ram_usage") && response_obj["data_ram_usage"].is_number_integer()) {
        int data_ram_usage = response_obj["data_ram_usage"].get<int>();
        network_status["data_ram_usage"] = data_ram_usage;
    }
    
    if (response_obj.contains("host") && response_obj["host"].is_string()) {
        std::string host = response_obj["host"].get<std::string>();
        network_status["host"] = QString::fromStdString(host);
    }
    
    if (response_obj.contains("peers") && response_obj["peers"].is_array()) {
        const auto& peers_array = response_obj["peers"];
        QVariantList peersList;
        
        for(const auto& peer : peers_array) {
            if(peer.is_object()) {
                QVariantMap peerObject;
                if(peer.contains("id") && peer["id"].is_string()) {
                    peerObject.insert("id", QString::fromStdString(peer["id"].get<std::string>()));
                }
                if(peer.contains("address") && peer["address"].is_string()) {
                    peerObject.insert("address", QString::fromStdString(peer["address"].get<std::string>()));
                }
                if(peer.contains("port") && peer["port"].is_number_integer()) {
                    peerObject.insert("port", peer["port"].get<int>());
                }
                if(peer.contains("status") && peer["status"].is_number_integer()) {
                    int status = peer["status"].get<int>();
                    peerObject.insert("status", status);
                    if(status == 0) {
                        peerObject.insert("status_str", "Dead");
                    }
                    if(status == 1) {
                        peerObject.insert("status_str", "Inactive");
                    }
                    if(status == 2) {
                        peerObject.insert("status_str", "Active");
                    }
                }
                //if(peer.contains("distance") && peer["distance"].is_()) {}
                peersList.append(peerObject);
            }
        }
        
        network_status["peers"] = peersList;
    }
    
    return network_status;
}

void laiin::Backend::clearHashTable() {
    if(!DaemonManager::isDaemonServerBound()) { return; }
    
    Client * client = Client::get_main_client();
    std::string response;
    client->clear(response);//std::cout << "Received response (clear): " << response << "\n";
}

