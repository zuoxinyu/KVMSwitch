import Foundation
import SwiftUI

struct Preset: Identifiable, Codable, Hashable {
    var id: UUID
    var name: String
    var settings: [String: UInt16]
    
    init(id: UUID = UUID(), name: String, settings: [String: UInt16]) {
        self.id = id
        self.name = name
        self.settings = settings
    }
}

class PresetManager: ObservableObject {
    @Published var presets: [Preset] = [] {
        didSet {
            save()
        }
    }
    
    init() {
        load()
    }
    
    func load() {
        if let data = UserDefaults.standard.data(forKey: "presets"),
           let decoded = try? JSONDecoder().decode([Preset].self, from: data) {
            presets = decoded
        }
    }
    
    func save() {
        if let data = try? JSONEncoder().encode(presets) {
            UserDefaults.standard.set(data, forKey: "presets")
        }
    }
}
