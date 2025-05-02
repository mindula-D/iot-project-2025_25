#!/usr/bin/env python3
# Smart Study Environment Analyzer - Automated Prediction Script
# This script runs every 15 minutes to update predictions for the next 6 hours

# General libraries
import pandas as pd
import numpy as np
import os
from datetime import datetime, timedelta
from math import sqrt

# MongoDB
import pymongo

# Machine learning libraries
from sklearn.preprocessing import MinMaxScaler
from sklearn.metrics import mean_squared_error, mean_absolute_error

# Deep learning libraries
import tensorflow as tf
from tensorflow.keras.models import Sequential, load_model
from tensorflow.keras.layers import LSTM, Dense, Dropout
from tensorflow.keras.callbacks import EarlyStopping, ModelCheckpoint

# Suppress TensorFlow warnings
import logging
logging.getLogger('tensorflow').setLevel(logging.ERROR)

# MongoDB connection details
CONNECTION_STRING = "mongodb+srv://it21291432:1234@cluster0.y104xxv.mongodb.net/?retryWrites=true&w=majority&appName=Cluster0"
DB_NAME = "study_environment_db"
SOURCE_COLLECTION = "study_env_new"
PREDICTION_COLLECTION = "study_env_predictions"

# Model parameters
SEQUENCE_LENGTH = 12
PREDICTION_HORIZON = 72  # 6 hours at 5-minute intervals

# Connect to MongoDB
def connect_to_mongodb():
    client = pymongo.MongoClient(CONNECTION_STRING)
    db = client[DB_NAME]
    source_collection = db[SOURCE_COLLECTION]
    prediction_collection = db[PREDICTION_COLLECTION]
    return source_collection, prediction_collection

# Extract data from MongoDB
def extract_data(collection, days=7):
    # Get data from the last 'days' days
    end_date = datetime.now()
    start_date = end_date - timedelta(days=days)
    
    # Query MongoDB
    query = {"timestamp": {"$gte": start_date, "$lte": end_date}}
    cursor = collection.find(query).sort("timestamp", 1)
    
    # Convert to DataFrame
    data = list(cursor)
    if not data:
        raise ValueError("No data found in the specified time range")
    
    df = pd.DataFrame(data)
    return df

# Prepare data for time series analysis
def prepare_time_series_data(df):
    # Convert MongoDB timestamp to pandas datetime
    df['timestamp'] = pd.to_datetime(df['timestamp'])
    
    # Set timestamp as index
    df = df.set_index('timestamp')
    
    # Select relevant features
    features = ['temperature', 'humidity', 'light_lux', 'sound_raw', 'study_quality_score']
    df = df[features]
    
    # Handle missing values
    df = df.interpolate(method='time')
    
    # Resample to regular intervals (5 minutes)
    df = df.resample('5T').mean().interpolate()
    
    return df

# Create time features
def create_time_features(df):
    # Add time-based features
    df = df.copy()
    df['hour'] = df.index.hour
    df['day_of_week'] = df.index.dayofweek
    df['is_weekend'] = df.index.dayofweek >= 5
    
    # One-hot encode categorical features
    df['is_weekend'] = df['is_weekend'].astype(int)
    
    return df

# Scale the data
def scale_data(df):
    scaler = MinMaxScaler()
    scaled_df = pd.DataFrame(scaler.fit_transform(df), 
                             columns=df.columns,
                             index=df.index)
    return scaled_df, scaler

# Create sequences for time series prediction
def create_sequences(data, seq_length=SEQUENCE_LENGTH):
    xs = []
    ys = []
    
    for i in range(len(data) - seq_length):
        x = data.iloc[i:(i + seq_length)].values
        y = data.iloc[i + seq_length].values
        xs.append(x)
        ys.append(y)
        
    return np.array(xs), np.array(ys)

# Build LSTM model
def build_lstm_model(input_shape, output_size):
    model = Sequential()
    
    # LSTM layers
    model.add(LSTM(64, activation='relu', return_sequences=True, input_shape=input_shape))
    model.add(Dropout(0.2))
    
    model.add(LSTM(32, activation='relu'))
    model.add(Dropout(0.2))
    
    # Output layer
    model.add(Dense(output_size))
    
    # Compile the model
    model.compile(optimizer='adam', loss='mse', metrics=['mae'])
    
    return model

# Prepare data for model
def prepare_data_for_model():
    # Connect to MongoDB
    collection, _ = connect_to_mongodb()
    
    # Extract data
    print("Extracting data from MongoDB...")
    df = extract_data(collection)
    print(f"Extracted {len(df)} records")
    
    # Prepare time series data
    print("Preparing time series data...")
    ts_df = prepare_time_series_data(df)
    print(f"Time series data shape: {ts_df.shape}")
    
    # Create time features
    print("Creating time features...")
    feature_df = create_time_features(ts_df)
    print(f"Feature data shape: {feature_df.shape}")
    
    # Scale the data
    print("Scaling data...")
    scaled_df, scaler = scale_data(feature_df)
    
    # Create sequences
    print("Creating sequences for model training...")
    X, y = create_sequences(scaled_df, SEQUENCE_LENGTH)
    print(f"X shape: {X.shape}, y shape: {y.shape}")
    
    # Split into train and test sets
    train_size = int(len(X) * 0.8)
    X_train, X_test = X[:train_size], X[train_size:]
    y_train, y_test = y[:train_size], y[train_size:]
    
    print(f"Training set: {X_train.shape}, Testing set: {X_test.shape}")
    
    return X_train, y_train, X_test, y_test, scaler, scaled_df.columns

# Train the model
def train_model(X_train, y_train, X_test, y_test):
    # Get input and output shapes
    input_shape = (X_train.shape[1], X_train.shape[2])
    output_size = y_train.shape[1]
    
    print(f"Input shape: {input_shape}, Output size: {output_size}")
    
    # Build the model
    model = build_lstm_model(input_shape, output_size)
    model.summary()
    
    # Set up callbacks
    early_stopping = EarlyStopping(monitor='val_loss', patience=10, restore_best_weights=True)
    model_checkpoint = ModelCheckpoint('best_model.h5', save_best_only=True, monitor='val_loss')
    
    # Train the model
    history = model.fit(
        X_train, y_train,
        epochs=20,  # Reduced for faster training
        batch_size=32,
        validation_data=(X_test, y_test),
        callbacks=[early_stopping, model_checkpoint],
        verbose=1
    )
    
    return model

# Predict future values
def predict_future(model, last_sequence, scaler, feature_names, num_steps=PREDICTION_HORIZON):
    future_predictions = []
    current_sequence = last_sequence.copy()
    
    for _ in range(num_steps):
        # Predict the next step
        next_step = model.predict(current_sequence)[0]
        future_predictions.append(next_step)
        
        # Update the sequence
        current_sequence = np.roll(current_sequence, -1, axis=1)
        current_sequence[0, -1, :] = next_step
    
    # Convert predictions to DataFrame
    future_pred_array = np.array(future_predictions)
    
    # If we have time features in our sequence, handle them separately
    n_time_features = 3  # hour, day_of_week, is_weekend
    
    # Get only the target variables (not the time features)
    target_features = feature_names[:-n_time_features] if len(feature_names) > 5 else feature_names
    
    # Inverse transform only the actual measurements
    measurements_pred = future_pred_array[:, :len(target_features)]
    
    # Create a dummy array with the right shape for inverse_transform
    full_features = np.zeros((measurements_pred.shape[0], len(feature_names)))
    full_features[:, :len(target_features)] = measurements_pred
    
    # Inverse transform
    measurements_pred_inv = scaler.inverse_transform(full_features)[:, :len(target_features)]
    
    # Create a DataFrame with the predictions
    future_df = pd.DataFrame(measurements_pred_inv, columns=target_features)
    
    return future_df

# Save predictions to MongoDB
def save_predictions_to_mongodb(predictions_df, prediction_collection):
    # Add timestamps to predictions
    start_time = datetime.now()
    predictions_df['predicted_at'] = start_time
    
    # Add timestamps for each predicted point (5-minute intervals)
    timestamps = []
    for i in range(len(predictions_df)):
        timestamps.append(start_time + timedelta(minutes=5 * i))
    
    predictions_df['prediction_timestamp'] = timestamps
    
    # Convert to records for MongoDB
    records = predictions_df.to_dict('records')
    
    # Insert each prediction individually
    for record in records:
        # Ensure MongoDB-compatible format
        record['prediction_timestamp'] = record['prediction_timestamp']
        record['predicted_at'] = record['predicted_at']
        record['is_prediction'] = True  # Flag to distinguish from actual readings
        
        # Add prediction document
        prediction_collection.insert_one(record)
    
    print(f"Saved {len(records)} prediction points to MongoDB")

# Main function
def main():
    print(f"Starting prediction update at {datetime.now()}")
    
    # Connect to MongoDB
    source_collection, prediction_collection = connect_to_mongodb()
    
    # Prepare data and train model
    X_train, y_train, X_test, y_test, scaler, feature_names = prepare_data_for_model()
    
    # Train model
    print("Training LSTM model...")
    model = train_model(X_train, y_train, X_test, y_test)
    
    # Get the last sequence for prediction
    last_sequence = X_test[-1:].copy()
    
    # Make future predictions (6 hours = 72 steps at 5-minute intervals)
    print("Predicting next 6 hours...")
    future_predictions = predict_future(model, last_sequence, scaler, feature_names)
    
    # Save predictions to MongoDB
    print("Saving predictions to MongoDB...")
    save_predictions_to_mongodb(future_predictions, prediction_collection)
    
    print(f"Prediction update completed at {datetime.now()}")

# Run the script
if __name__ == "__main__":
    main()
